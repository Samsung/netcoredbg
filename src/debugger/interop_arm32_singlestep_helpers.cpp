// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_arm32_singlestep_helpers.h"
#include "debugger/interop_brk_helpers.h"

#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <functional>
#include "utils/logger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

struct sw_singlestep_nextpc_t
{
    std::uintptr_t addr = 0;
    bool isThumb = false;

    sw_singlestep_nextpc_t(std::uintptr_t addr_, bool isThumb_) :
        addr(addr_),
        isThumb(isThumb_)
    {}
};

constexpr int REG_SP   = 13;
constexpr int REG_LR   = 14;
constexpr int REG_PC   = 15;
constexpr int REG_CPSR = 16;

// Return trueif the processor is currently executing in Thumb mode
static bool IsExecutingThumb(const user_regs_struct &regs)
{
    constexpr int REG_CPSR = 16;
    std::uintptr_t cpsr = regs.uregs[REG_CPSR];

  // FIXME for `M profiles` (Cortex-M), XPSR_T_BIT must be used instead.
  // CPSR_T_BIT 0x20 // 5 bit
  // XPSR_T_BIT 0x01000000 // 25 bit
  return cpsr & 0x20;
}

template<typename T>
bool GetDataFromMemory(pid_t pid, std::uintptr_t addr, T &result)
{
    errno = 0;
    word_t wData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)addr, nullptr);
    if (errno != 0)
    {
        LOGE("Ptrace peekdata error: %s", strerror(errno));
        return false;
    }

    // Note, little-endian only architectures are supported now.
    assert(sizeof(word_t) % sizeof(T) == 0); // result with type `T` must be element of `word_t`-size array
    result = ((T*)&wData)[0]; // just get first element of array
    return true;
}

static inline std::uint32_t MakeSubmask(std::uint32_t x)
{
    return ((std::uint32_t)1 << (x + 1)) - 1;
}
static inline std::uint32_t GetBits(std::uint32_t val, std::uint32_t first, std::uint32_t last)
{
    return (val >> first) & MakeSubmask(last - first);
}
static inline std::uint32_t GetBit(std::uint32_t val, std::uint32_t number)
{
    return (val >> number) & 1;
}
static inline std::uint32_t GetSBits(std::uint32_t val, std::uint32_t first, std::uint32_t last)
{
    return (std::int32_t)GetBits(val, first, last) | ((std::int32_t)GetBit(val, last) * ~MakeSubmask(last - first));
}
static inline std::uintptr_t CalculateBranchDest(std::uint32_t addr, std::uint32_t instr)
{
    // From "4.4 Branch and Branch with Link (B, BL)":
    // Branch instructions contain a signed 2’s complement 24 bit offset. This is shifted left
    // two bits, sign extended to 32 bits, and added to the PC. The instruction can therefore
    // specify a branch of +/- 32Mbytes. The branch offset must take account of the prefetch
    // operation, which causes the PC to be 2 words (8 bytes) ahead of the current instruction.
    return addr + 8 + (GetSBits(instr, 0, 23) << 2);
}

constexpr std::uint32_t INST_AL = 0xe; // allways
constexpr std::uint32_t INST_NV = 0xf; // unconditional/allways

constexpr std::uint32_t FLAG_N = 0x80000000;
constexpr std::uint32_t FLAG_Z = 0x40000000;
constexpr std::uint32_t FLAG_C = 0x20000000;
constexpr std::uint32_t FLAG_V = 0x10000000;

static bool IsConditionTrue(std::uint32_t cond, std::uint32_t regPS)
{
    if (cond == INST_AL || cond == INST_NV)
        return true;

    static std::vector<std::function<bool(std::uint32_t)>> conditionLogic
    {
        [](std::uint32_t regPS) {return ((regPS & FLAG_Z) != 0);}, // INST_EQ = 0x0
        [](std::uint32_t regPS) {return ((regPS & FLAG_Z) == 0);}, // INST_NE = 0x1
        [](std::uint32_t regPS) {return ((regPS & FLAG_C) != 0);}, // INST_CS = 0x2
        [](std::uint32_t regPS) {return ((regPS & FLAG_C) == 0);}, // INST_CC = 0x3
        [](std::uint32_t regPS) {return ((regPS & FLAG_N) != 0);}, // INST_MI = 0x4
        [](std::uint32_t regPS) {return ((regPS & FLAG_N) == 0);}, // INST_PL = 0x5
        [](std::uint32_t regPS) {return ((regPS & FLAG_V) != 0);}, // INST_VS = 0x6
        [](std::uint32_t regPS) {return ((regPS & FLAG_V) == 0);}, // INST_VC = 0x7
        [](std::uint32_t regPS) {return ((regPS & (FLAG_C | FLAG_Z)) == FLAG_C);}, // INST_HI = 0x8
        [](std::uint32_t regPS) {return ((regPS & (FLAG_C | FLAG_Z)) != FLAG_C);}, // INST_LS = 0x9
        [](std::uint32_t regPS) {return (((regPS & FLAG_N) == 0) == ((regPS & FLAG_V) == 0));}, // INST_GE = 0xa
        [](std::uint32_t regPS) {return (((regPS & FLAG_N) == 0) != ((regPS & FLAG_V) == 0));}, // INST_LT = 0xb
        [](std::uint32_t regPS) {return (((regPS & FLAG_Z) == 0) && (((regPS & FLAG_N) == 0) == ((regPS & FLAG_V) == 0)));}, // INST_GT = 0xc
        [](std::uint32_t regPS) {return (((regPS & FLAG_Z) != 0) || (((regPS & FLAG_N) == 0) != ((regPS & FLAG_V) == 0)));}  // INST_LE = 0xd
    };

    if (conditionLogic.size() < cond)
        return conditionLogic[cond](regPS);

    return true;
}

static std::uint32_t ShiftRegValue(const user_regs_struct &regs, std::uint32_t inst, bool carry, std::uint32_t regPC)
{
    // if 4 bit == 0
    //      11-7 bits - shift amount, 5 bit unsigned integer
    // if 4 bit == 1
    //      11-8 bits - shift register
    //      7 bit is `0`
    //
    // 6-5 bits  - shift type:
    //      00 = logical left
    //      01 = logical right
    //      10 = arithmetic right
    //      11 = rotate right
    // 3-0 bits  - offset register

    std::uint32_t shift;
    if (GetBit(inst, 4))
    {
        int shiftReg = GetBits(inst, 8, 11);
        shift = (shiftReg == REG_PC ? regPC + 8 : (std::uint32_t)(regs.uregs[shiftReg])) & 0xff;
    }
    else
    {
        shift = GetBits(inst, 7, 11);
    }

    int offsetReg = GetBits(inst, 0, 3);
    std::uint32_t result = (offsetReg == REG_PC ? (regPC + (GetBit(inst, 4) ? 12 : 8)) : (std::uint32_t)(regs.uregs[offsetReg]));

    static std::vector<std::function<void(std::uint32_t,bool,std::uint32_t&)>> shiftLogic
    {
        [](std::uint32_t shift, bool, std::uint32_t &result) { result = shift >= 32 ? 0 : result << shift;}, // LSL = 0
        [](std::uint32_t shift, bool, std::uint32_t &result) { result = shift >= 32 ? 0 : result >> shift;}, // LSR = 1
        [](std::uint32_t shift, bool, std::uint32_t &result)
        {
            if (shift >= 32)
                shift = 31;

            result = ((result & 0x80000000L) ? ~((~result) >> shift) : result >> shift);
        },                                                                                                   // ASR = 2
        [](std::uint32_t shift, bool carry, std::uint32_t &result)
        {
            shift &= 31;
            if (shift == 0)
                result = (result >> 1) | (carry ? 0x80000000L : 0);
            else
                result = (result >> shift) | (result << (32 - shift));
        }                                                                                                     // ROR/RRX = 3
    };

    std::uint32_t shiftType = GetBits(inst, 5, 6);
    if (shiftLogic.size() < shiftType)
        shiftLogic[shiftType](shift, carry, result);

    return result;
}

static bool ArmUnconditional_Branches(std::uintptr_t currentPC, std::uint32_t currentInstr, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    nextPC = CalculateBranchDest(currentPC, currentInstr);
    nextPC |= GetBit(currentInstr, 24) << 1;
    switchToThumbCode = true;
    return true;
};

static bool ArmUnconditional_CoprocessorOperations(std::uintptr_t, std::uint32_t currentInstr, std::uintptr_t&, bool&)
{
    if (GetBits(currentInstr, 12, 15) == REG_PC)
    {
        LOGE("Failed next PC calculation");
        return false;
    }
    return true;
};

static bool ArmConditionTrue_Miscellaneous(pid_t, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                                           std::uint32_t currentInstr, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    // Multiply and Multiply-Accumulate (MUL, MLA)
    // 4.7.1 Operand restrictions
    //  R15 must not be used as an operand or as the destination register.
    if (GetBits(currentInstr, 22, 27) == 0 && GetBits(currentInstr, 4, 7) == 9)
        return true;

    // Multiply Long and Multiply-Accumulate Long (MULL, MLAL)
    // 4.8.1 Operand restrictions
    //  R15 must not be used as an operand or as a destination register.
    if (GetBits(currentInstr, 23, 27) == 1 && GetBits(currentInstr, 4, 7) == 9)
        return true;

    // Single Data Swap (SWP)
    // 4.12.2 Use of R15
    //  Do not use R15 as an operand (Rd, Rn or Rs) in a SWP instruction.
    if (GetBits(currentInstr, 23, 27) == 0x2 && GetBits(currentInstr, 20, 21) == 0 && GetBits(currentInstr, 4, 11) == 9)
        return true;

    // BX register, BLX register
    if (GetBits(currentInstr, 4, 27) == 0x12fff1 ||
        GetBits(currentInstr, 4, 27) == 0x12fff3)
    {
        std::uintptr_t rn = GetBits(currentInstr, 0, 3);
        nextPC = ((rn == REG_PC) ? (currentPC + 8) : std::uintptr_t(regs.uregs[rn]));
        // This instruction also permits the instruction set to be exchanged. When the instruction is executed,
        // the value of Rn[0] determines whether the instruction stream will be decoded as ARM or THUMB instructions.
        switchToThumbCode = nextPC & 1;
        nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address.
        return true;
    }

    // Halfword and Signed Data Transfer (LDRH/STRH/LDRSB/LDRSH)
    if ((GetBits(currentInstr, 25, 27) == 0 && GetBit(currentInstr, 22) == 0 && GetBits(currentInstr, 7, 11) == 1 && GetBit(currentInstr, 4) == 1) || // register offset
        (GetBits(currentInstr, 25, 27) == 0 && GetBit(currentInstr, 22) == 1 && GetBit(currentInstr, 7) == 1 && GetBit(currentInstr, 4) == 1))        // immediate offset
    {
        // TODO (load from memory)
        if (GetBits(currentInstr, 12, 15) == REG_PC && GetBit(currentInstr, 21) == 1)
        {
            LOGE("Load PC register from memory for LDRH/LDRSB/LDRSH not implemented.");
            return false;
        }
        // 4.10.5 Use of R15
        //  Write-back should not be specified if R15 is specified as the base register (Rn).
        if (GetBits(currentInstr, 16, 19) == REG_PC && GetBit(currentInstr, 21) == 1)
            return false;

        return true;
    }

    // Parsing of data processing / PSR transfer instructions.

    // if dest register is not PC, just leave
    if (GetBits(currentInstr, 12, 15) != REG_PC)
        return true;

    bool carry = (currentPS & FLAG_C) == FLAG_C;
    std::uintptr_t rn = GetBits(currentInstr, 16, 19);
    std::uintptr_t operand1 = ((rn == REG_PC) ? (currentPC + 8) : std::uintptr_t(regs.uregs[rn]));
    std::uintptr_t operand2 = 0;

    if (GetBit(currentInstr, 25))
    {
        std::uint32_t immval = GetBits(currentInstr, 0, 7);
        std::uint32_t rotate = 2 * GetBits(currentInstr, 8, 11);
        operand2 = ((immval >> rotate) | (immval << (32 - rotate)));
    }
    else // operand 2 is a shifted register.
    {
        operand2 = ShiftRegValue(regs, currentInstr, carry, currentPC);
    }

    static std::vector<std::function<std::uintptr_t(std::uintptr_t,std::uintptr_t,std::uintptr_t, bool)>> dataOperations
    {
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 & operand2; }, // and = 0x0
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 ^ operand2; }, // eor = 0x1
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 - operand2; }, // sub = 0x2
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand2 - operand1; }, // rsb = 0x3
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 + operand2; }, // add = 0x4
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool carry) { return operand1 + operand2 + (carry ? 1 : 0); }, // adc = 0x5
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool carry) { return operand1 - operand2 + (carry ? 1 : 0); }, // sbc = 0x6
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool carry) { return operand2 - operand1 + (carry ? 1 : 0); }, // rsc = 0x7
        // Note, we don't set condition codes, just "do nothing" for this OpCodes.
        [](std::uintptr_t nextPC, std::uintptr_t, std::uintptr_t, bool) { return nextPC; }, // tst = 0x8
        [](std::uintptr_t nextPC, std::uintptr_t, std::uintptr_t, bool) { return nextPC; }, // teq = 0x9
        [](std::uintptr_t nextPC, std::uintptr_t, std::uintptr_t, bool) { return nextPC; }, // cmp = 0xa
        [](std::uintptr_t nextPC, std::uintptr_t, std::uintptr_t, bool) { return nextPC; }, // cmn = 0xb
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 | operand2; }, // orr = 0xc
        [](std::uintptr_t, std::uintptr_t, std::uintptr_t operand2, bool) { return operand2; }, // mov = 0xd
        [](std::uintptr_t, std::uintptr_t operand1, std::uintptr_t operand2, bool) { return operand1 & ~operand2; }, // bic = 0xe
        [](std::uintptr_t, std::uintptr_t, std::uintptr_t operand2, bool) { return ~operand2; }, // mvn = 0xf
    };

    nextPC = dataOperations[GetBits(currentInstr, 21, 24)](nextPC, operand1, operand2, carry);
    nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address. Note, we don't support M-profiles here.
    return true;
};

static bool ArmConditionTrue_MemoryOperations(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                                              std::uint32_t currentInstr, std::uintptr_t &nextPC, bool&)
{
    // We care about LDR only here.
    if (GetBits(currentInstr, 25, 27) == 0x3 && GetBit(currentInstr, 4) == 1)
        return true;

    if (GetBit(currentInstr, 20) && GetBits(currentInstr, 12, 15) == REG_PC) // LDR (in case PC is dest)
    {
        // 1 - transfer byte quantity
        if (GetBit(currentInstr, 22) == 1)
        {
            LOGE("Failed next PC calculation");
            return false;
        }

        std::uint32_t baseReg = GetBits(currentInstr, 16, 19);
        std::uintptr_t baseData = ((baseReg == REG_PC) ? (currentPC + 8) : std::uintptr_t(regs.uregs[baseReg]));

        if (GetBit(currentInstr, 24)) // pre-index
        {
            bool carry = (currentPS & FLAG_C) == FLAG_C;
            std::uint32_t offset = (GetBit(currentInstr, 25) ?
                ShiftRegValue(regs, currentInstr, carry, currentPC) : // shift
                GetBits(currentInstr, 0, 11)); // 12-bit immediate

            if (GetBit(currentInstr, 23))
                baseData += offset; // up
            else
                baseData -= offset; // down
        }

        if (!GetDataFromMemory(pid, baseData, nextPC))
        {
            LOGE("Failed next PC calculation");
            return false;
        }
    }

    return true;
};

static bool ArmConditionTrue_MultipleMemoryOperations(pid_t pid, const user_regs_struct &regs, std::uintptr_t, std::uintptr_t,
                                                      std::uint32_t currentInstr, std::uintptr_t &nextPC, bool&)
{
    if (GetBit(currentInstr, 20) && GetBit(currentInstr, REG_PC)) // LDM (in case PC also included in register list)
    {
        int offset = 0;
        if (GetBit(currentInstr, 23)) // up
        {
            unsigned long reglist = GetBits(currentInstr, 0, 14); // in this case reglist is "array of bits"
            offset = __builtin_popcountl(reglist) * 4; // count offset for all registers that are set in register list
            if (GetBit(currentInstr, 24)) // pre-index
                offset += 4; // count offset for PC too
        }
        else if (GetBit(currentInstr, 24)) // down + pre-index
        {
            offset = -4; // count offset for PC only in this case
        }

        std::uint32_t baseReg = GetBits(currentInstr, 16, 19);
        std::uintptr_t addr = std::uintptr_t(regs.uregs[baseReg]) + offset;
        if (!GetDataFromMemory(pid, addr, nextPC))
        {
            LOGE("Failed next PC calculation");
            return false;
        }
    }

    return true;
};

static bool ArmConditionTrue_Branches(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t currentPC,
                                      std::uint32_t currentInstr, std::uintptr_t &nextPC, bool&)
{
    nextPC = CalculateBranchDest(currentPC, currentInstr);
    return true;
};

// Get next possible addresses for Arm instruction subset.
static bool GetArmCodeNextPCs(pid_t pid, const user_regs_struct &regs, std::vector<sw_singlestep_nextpc_t> &swSingleStepNextPCs)
{
    std::uintptr_t currentPC = std::uintptr_t(regs.uregs[REG_PC]);
    std::uintptr_t nextPC = currentPC + 4; // default PC changes
    bool switchToThumbCode = false;

    std::uint32_t currentInstr = 0;
    if (!GetDataFromMemory<std::uint32_t>(pid, currentPC, currentInstr))
        return false;

    std::uintptr_t currentPS = regs.uregs[REG_CPSR];

    constexpr unsigned unconditionalOpsShift = 0xa;
    static std::vector<std::function<bool(std::uintptr_t, std::uint32_t, std::uintptr_t&, bool&)>> unconditionalOperations
    {
        // branches
        ArmUnconditional_Branches, // 0xa - branch and change to Thumb
        ArmUnconditional_Branches, // 0xb - branch & link and change to Thumb
        // coprocessor operations
        ArmUnconditional_CoprocessorOperations, // 0xc
        ArmUnconditional_CoprocessorOperations, // 0xd
        ArmUnconditional_CoprocessorOperations  // 0xe
    };

    static std::vector<std::function<bool(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t, std::uint32_t, std::uintptr_t&, bool&)>> conditionTrueOperations
    {
        // miscellaneous instructions (multiply, swap, branch and exchange, data operations)
        ArmConditionTrue_Miscellaneous, // 0x0
        ArmConditionTrue_Miscellaneous, // 0x1
        ArmConditionTrue_Miscellaneous, // 0x2
        ArmConditionTrue_Miscellaneous, // 0x3
        // memory operations
        ArmConditionTrue_MemoryOperations, // 0x4
        ArmConditionTrue_MemoryOperations, // 0x5
        ArmConditionTrue_MemoryOperations, // 0x6
        ArmConditionTrue_MemoryOperations, // 0x7
        // block/multiple memory operations
        ArmConditionTrue_MultipleMemoryOperations, // 0x8
        ArmConditionTrue_MultipleMemoryOperations, // 0x9
        // branches
        ArmConditionTrue_Branches, // 0xa - branch
        ArmConditionTrue_Branches, // 0xb - branch & link
        // coprocessor operations (do nothing)
        // 0xc
        // 0xd
        // 0xe
        // system calls (do nothing)
        // 0xf - TODO care about SIGRETURN/RT_SIGRETURN syscalls.
    };

    if (GetBits(currentInstr, 28, 31) == INST_NV)
    {
        unsigned op = GetBits(currentInstr, 24, 27);
        if (op > unconditionalOpsShift && op < unconditionalOpsShift + unconditionalOperations.size())
        {
            if (!unconditionalOperations[op - unconditionalOpsShift](currentPC, currentInstr, nextPC, switchToThumbCode))
                return false;
        }

        // Note, Linux kernel could offers some helpers/intrisics in a high page that we can't read (and write).
        // For BL and BLX move to address of following instruction, in case tail called functions return to the address in LR.
        if (nextPC > 0xffff0000)
        {
            switchToThumbCode = false;
            if (op == 0xb) // BLX <label>
                nextPC = currentPC + 4;
            else
                nextPC = std::uintptr_t(regs.uregs[REG_LR]);
        }
    }
    else if (IsConditionTrue(GetBits(currentInstr, 28, 31), currentPS))
    {
        unsigned op = GetBits(currentInstr, 24, 27);
        if (op < conditionTrueOperations.size())
        {
            if (!conditionTrueOperations[op](pid, regs, currentPS, currentPC, currentInstr, nextPC, switchToThumbCode))
                return false;
        }

        // Note, Linux kernel could offers some helpers/intrisics in a high page that we can't read (and write).
        // For BL and BLX move to address of following instruction, in case tail called functions return to the address in LR.
        if (nextPC > 0xffff0000)
        {
            switchToThumbCode = false;
            if (op == 0xb || // BL <label>
                GetBits(currentInstr, 4, 27) == 0x12fff3) // BLX register
                nextPC = currentPC + 4;
            else
                nextPC = std::uintptr_t(regs.uregs[REG_LR]);
        }
    }

    swSingleStepNextPCs.emplace_back(nextPC, switchToThumbCode);
    return true;
}

static std::uint32_t ThumbInsnructionSize(std::uint16_t inst1)
{
    return ((inst1 & 0xe000) == 0xe000 && (inst1 & 0x1800) != 0) ? 4 : 2;
}

static std::uint32_t ThumbAdvanceITState(std::uint32_t itstate)
{
    // IT[7:5] Holds the base condition for the current IT block. The base condition is the top 3 bits of the condition specified by the IT instruction.
    // IT[4:0] The size of the IT block. This is the number of instructions that are to be conditionally executed.
    // All we need here is decrement IT[4:0] part by 1.
    itstate = (itstate & 0xe0) | ((itstate << 1) & 0x1f);

    // In case IT[3:0] == 0, mean IT block is finished, clear the state.
    // See https://developer.arm.com/documentation/ddi0406/b/Application-Level-Architecture/Application-Level-Programmers--Model/Execution-state-registers/ITSTATE?lang=en
    // "Table 2.2. Effect of IT execution state bits" for more info.
    return ((itstate & 0x0f) == 0) ? 0 : itstate;
}

static bool GetThumbConditionalBlockNextPCs(pid_t pid, std::uintptr_t currentPS, std::uintptr_t currentPC, std::uint16_t inst1, std::vector<sw_singlestep_nextpc_t> &swSingleStepNextPCs)
{
    // In Linux, breakpoint is illegal instruction. IT can disable illegal instruction execution.
    // This mean, we could never reach this breakpoint. Plus, conditional instructions can change flags,
    // that change execution route, this mean we could need set 2 breakpoints and care about this case too.

    if ((inst1 & 0xff00) == 0xbf00 && (inst1 & 0x000f) != 0) // thumb 16-bit "If-Then" instructions
    {
        std::uint32_t ITState = inst1 & 0x00ff;
        std::uintptr_t nextPC = currentPC + ThumbInsnructionSize(inst1);

        while (ITState != 0 && !IsConditionTrue(ITState >> 4, currentPS))
        {
            if (!GetDataFromMemory<std::uint16_t>(pid, nextPC, inst1))
                return false;

            nextPC += ThumbInsnructionSize(inst1);
            ITState = ThumbAdvanceITState(ITState);
        }

        swSingleStepNextPCs.emplace_back(nextPC, true);
        return true;
    }

    // https://developer.arm.com/documentation/ddi0406/b/System-Level-Architecture/The-System-Level-Programmers--Model/ARM-processor-modes-and-core-registers/Program-Status-Registers--PSRs-
    // IT[7:0], CPSR bits [15:10,26:25]
    std::uint32_t ITState = ((currentPS >> 8) & 0xfc) | ((currentPS >> 25) & 0x3);

    if (ITState == 0)
        return true;

    if (!IsConditionTrue(ITState >> 4, currentPS))
    {
        // Advance to the next executed instruction till this block ends.
        std::uintptr_t nextPC = currentPC + ThumbInsnructionSize(inst1);
        ITState = ThumbAdvanceITState(ITState);

        while (ITState != 0 && !IsConditionTrue(ITState >> 4, currentPS))
        {
            if (!GetDataFromMemory<std::uint16_t>(pid, nextPC, inst1))
                return false;

            nextPC += ThumbInsnructionSize(inst1);
            ITState = ThumbAdvanceITState(ITState);
        }

        swSingleStepNextPCs.emplace_back(nextPC, true);
        return true;
    }

    if ((ITState & 0x0f) == 0x08) // current instruction is the last instruction of conditional block
        return true;

    // Current instruction is conditional instruction that may change flags.
    // Note, we can't predict what the next executed instruction will be.
    std::uintptr_t nextPC = currentPC + ThumbInsnructionSize(inst1);
    // Set a breakpoint on the following instruction
    swSingleStepNextPCs.emplace_back(nextPC, true);

    ITState = ThumbAdvanceITState(ITState);
    std::uint32_t negatedInitialCondition = (ITState >> 4) & 1;
    // "skip" all instruction with the same condition or till this block ends.
    do
    {
        if (!GetDataFromMemory<std::uint16_t>(pid, nextPC, inst1))
            return false;

        nextPC += ThumbInsnructionSize(inst1);
        ITState = ThumbAdvanceITState(ITState);
    }
    while (ITState != 0 && ((ITState >> 4) & 1) == negatedInitialCondition);

    swSingleStepNextPCs.emplace_back(nextPC, true);
    return true;
}

static bool Thumb16_Default(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t, std::uint16_t, std::uintptr_t&, bool&)
{
    return true;
};

static bool Thumb16_BranchExchangeAndDataProcessing(pid_t, const user_regs_struct &regs, std::uintptr_t, std::uintptr_t currentPC,
                                                    std::uint16_t inst1, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    if ((inst1 & 0xff00) == 0x4700) // BX REG, BLX REG
    {
        if (GetBits(inst1, 3, 6) == REG_PC)
        {
            const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
            nextPC = prefetchedPC;
            switchToThumbCode = false;
        }
        else
        {
            std::uint32_t sourceReg = GetBits(inst1, 3, 6);
            nextPC = std::uintptr_t(regs.uregs[sourceReg]);
            switchToThumbCode = nextPC & 1;
            nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address.
        }
    }
    else if ((inst1 & 0xff87) == 0x4687) // MOV PC, REG
    {
        if (GetBits(inst1, 3, 6) == REG_PC)
        {
            const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
            nextPC = prefetchedPC;
        }
        else
        {
            std::uint32_t sourceReg = GetBits(inst1, 3, 6);
            nextPC = std::uintptr_t(regs.uregs[sourceReg]);
            nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address.
        }
    }

    return true;
};

static bool Thumb16_Miscellaneous(pid_t pid, const user_regs_struct &regs, std::uintptr_t, std::uintptr_t currentPC,
                                  std::uint16_t inst1, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    if ((inst1 & 0xff00) == 0xbd00) // POP {reglist, PC}
    {
        // Count offset for all registers that are set in register list. Note, PC stored above all of the other registers.
        int offset = __builtin_popcountl(GetBits(inst1, 0, 7)) * 4;
        std::uintptr_t regSP = std::uintptr_t(regs.uregs[REG_SP]);
        if (!GetDataFromMemory<std::uint32_t>(pid, regSP + offset, nextPC))
            return false;
        // Bit[0] of the loaded value determines whether execution continues after this branch in ARM state or in Thumb state.
        if ((nextPC & 1) == 0)
            switchToThumbCode = false;
        else
            nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address.
    }
    else if ((inst1 & 0xf500) == 0xb100) // CBZ or CBNZ (Compare and Branch on Zero, Compare and Branch on Non-Zero)
    {
        std::uintptr_t operandReg = std::uintptr_t(regs.uregs[GetBits(inst1, 0, 2)]);

        if ((GetBit(inst1, 11) && operandReg != 0) ||
            (!GetBit(inst1, 11) && operandReg == 0))
        {
            int imm = (GetBit(inst1, 9) << 6) + (GetBits(inst1, 3, 7) << 1);
            const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
            nextPC = prefetchedPC + imm;
        }
    }

    return true;
};

static bool Thumb16_ConditionalBranch(pid_t, const user_regs_struct&, std::uintptr_t currentPS, std::uintptr_t currentPC,
                                      std::uint16_t inst1, std::uintptr_t &nextPC, bool&)
{
    if ((inst1 & 0xf000) == 0xd000) // Conditional branch
    {
        std::uint32_t cond = GetBits(inst1, 8, 11);
        if (cond == INST_NV) // syscall
        {
            // TODO care about SIGRETURN/RT_SIGRETURN syscalls.
        }
        else if (IsConditionTrue(cond, currentPS))
        {
            const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
            nextPC = prefetchedPC + (GetSBits(inst1, 0, 7) << 1);
        }
    }

    return true;
};

static bool Thumb16_UnconditionalBranch(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t currentPC,
                                        std::uint16_t inst1, std::uintptr_t &nextPC, bool&)
{
    if ((inst1 & 0xf800) == 0xe000) // unconditional branch
    {
        const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
        nextPC = prefetchedPC + (GetSBits(inst1, 0, 10) << 1);
    }

    return true;
};

static bool Thumb32_BranchesMiscControl(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    if ((inst2 & 0x1000) != 0 || (inst2 & 0xd001) == 0xc000) // B, BL, BLX
    {
        int imm1 = GetSBits(inst1, 0, 10);
        int imm2 = GetBits(inst2, 0, 10);
        int j1 = GetBit(inst2, 13);
        int j2 = GetBit(inst2, 11);

        // I1 = NOT(J1 EOR S); I2 = NOT(J2 EOR S);
        // imm32 = SignExtend(S:I1:I2:imm10:imm11:'0', 32);
        std::uint32_t offset = ((imm1 << 12) + (imm2 << 1));
        offset ^= ((!j1) << 23) | ((!j2) << 22);
        const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
        nextPC = prefetchedPC + offset;

        if (GetBit(inst2, 12) == 0) // BLX
        {
            switchToThumbCode = false;
            // ARM Architecture Reference Manual Thumb-2 Supplement
            // 4.6.18 BL, BLX (immediate)
            //  For BLX (encoding T2), the assembler calculates the required value of the offset from the
            //  Align(PC,4) value of the BLX instruction to this label, then selects an encoding that will
            //  set imm32 to that offset. A
            nextPC = nextPC & 0xfffffffc;
        }
    }
    else if (inst1 == 0xf3de && (inst2 & 0xff00) == 0x3f00) // SUBS PC, LR, #imm8
    {
        nextPC = std::uintptr_t(regs.uregs[REG_LR]);
        // imm32 = ZeroExtend(imm8, 32);
        nextPC -= inst2 & 0x00ff; // 8-bit immediate constant
    }
    else if ((inst2 & 0xd000) == 0x8000 && (inst1 & 0x0380) != 0x0380) // conditional branch
    {
        if (IsConditionTrue(GetBits(inst1, 6, 9), currentPS))
        {
            int sign = GetSBits(inst1, 10, 10);
            int imm1 = GetBits(inst1, 0, 5);
            int imm2 = GetBits(inst2, 0, 10);
            int j1 = GetBit(inst2, 13);
            int j2 = GetBit(inst2, 11);

            // imm32 = SignExtend(S:J2:J1:imm6:imm11:'0', 32);
            std::uint32_t offset = (sign << 20) + (j2 << 19) + (j1 << 18);
            offset += (imm1 << 12) + (imm2 << 1);
            const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
            nextPC = prefetchedPC + offset;
        }
    }

    return true;
}

static bool Thumb32_LDM(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    auto loadPC = [&](std::int32_t offset)
    {
        int baseReg = GetBits(inst1, 0, 3);
        std::uintptr_t addr = std::uintptr_t(regs.uregs[baseReg]);
        if (!GetDataFromMemory<std::uint32_t>(pid, addr + offset, nextPC))
            return false;

        if ((nextPC & 1) == 0)
            switchToThumbCode = false;
        else
            nextPC = nextPC & ~(std::uintptr_t(1)); // Remove useless bits from address.

        return true;
    };

    if (GetBit(inst1, 7) && !GetBit(inst1, 8)) // LDMIA
    {
        std::int32_t offset = __builtin_popcountl(inst2) * 4 - 4;
        if (!loadPC(offset))
            return false;
    }
    else if (!GetBit(inst1, 7) && GetBit(inst1, 8)) // LDMDB
    {
        if (!loadPC(-4))
            return false;
    }

    return true;
}

static bool Thumb32_RFE(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    auto loadPC = [&](std::int32_t offset)
    {
        int baseReg = GetBits(inst1, 0, 3);
        std::uintptr_t addr = std::uintptr_t(regs.uregs[baseReg]);
        if (!GetDataFromMemory<std::uint32_t>(pid, addr + offset, nextPC))
            return false;

        std::uint32_t nextCPSR = 0;
        if (!GetDataFromMemory<std::uint32_t>(pid, addr + offset + 4, nextCPSR))
            return false;

        // FIXME for `M profiles` (Cortex-M), XPSR_T_BIT must be used instead.
        // CPSR_T_BIT 0x20 // 5 bit
        // XPSR_T_BIT 0x01000000 // 25 bit
        switchToThumbCode = nextCPSR & 0x20;

        return true;
    };

    if (GetBit(inst1, 7) && GetBit(inst1, 8)) // RFEIA
    {
        if (!loadPC(0))
            return false;
    }
    else if (!GetBit(inst1, 7) && !GetBit(inst1, 8)) // RFEDB
    {
        if (!loadPC(-8))
            return false;
    }

    return true;
}

static bool Thumb32_MOV(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    if (GetBits(inst2, 8, 11) == REG_PC) // only if <Rd> is PC
    {
        int srcReg = GetBits(inst2, 0, 3);
        nextPC = std::uintptr_t(regs.uregs[srcReg]);
    }

    return true;
}

static bool Thumb32_LDR(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    int rn = GetBits(inst1, 0, 3);
    std::uintptr_t base = std::uintptr_t(regs.uregs[rn]);

    auto loadPC = [&]()
    {
        return GetDataFromMemory<std::uint32_t>(pid, base, nextPC);
    };

    if (rn == REG_PC)
    {
        base = (base + 4) & ~(std::uintptr_t)0x3;
        if (GetBit(inst1, 7))
            base += GetBits(inst2, 0, 11);
        else
            base -= GetBits(inst2, 0, 11);

        return loadPC();
    }
    else if (GetBit(inst1, 7))
    {
        base += GetBits(inst2, 0, 11); // imm12
        return loadPC();
    }
    else if (GetBit(inst2, 11))
    {
        if (GetBit(inst2, 10))
        {
            if (GetBit(inst2, 9))
                base += GetBits(inst2, 0, 7);
            else
                base -= GetBits(inst2, 0, 7);
        }
        return loadPC();
    }
    else if ((inst2 & 0x0fc0) == 0x0000)
    {
        int shift = GetBits(inst2, 4, 5);
        int rm = GetBits(inst2, 0, 3);
        base += std::uintptr_t(regs.uregs[rm]) << shift;
        return loadPC();
    }

    return true;
}

static bool Thumb32_TBB(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    std::uintptr_t table;
    std::uintptr_t tableReg = GetBits(inst1, 0, 3);
    if (tableReg == REG_PC)
        table = currentPC + 4;
    else
        table = std::uintptr_t(regs.uregs[tableReg]);

    std::uintptr_t offset = std::uintptr_t(regs.uregs[GetBits(inst2, 0, 3)]);

    std::uint8_t tmp = 0;
    if (!GetDataFromMemory<std::uint8_t>(pid, table + offset, tmp))
        return false;

    std::uintptr_t length = 2 * tmp;
    const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
    nextPC = prefetchedPC + length;

    return true;
}

static bool Thumb32_TBH(pid_t pid, const user_regs_struct &regs, std::uintptr_t currentPS, std::uintptr_t currentPC,
                        std::uint16_t inst1, std::uint16_t inst2, std::uintptr_t &nextPC, bool &switchToThumbCode)
{
    std::uintptr_t table;
    std::uintptr_t tableReg = GetBits(inst1, 0, 3);
    if (tableReg == REG_PC)
        table = currentPC + 4;
    else
        table = std::uintptr_t(regs.uregs[tableReg]);

    std::uintptr_t offset = 2 * std::uintptr_t(regs.uregs[GetBits(inst2, 0, 3)]);

    std::uint16_t tmp = 0;
    if (!GetDataFromMemory<std::uint16_t>(pid, table + offset, tmp))
        return false;

    std::uintptr_t length = 2 * tmp;
    const std::uintptr_t prefetchedPC = currentPC + 4; // PC after prefetch
    nextPC = prefetchedPC + length;

    return true;
}

static bool FixThumbCodeNextPCs(pid_t pid, const user_regs_struct &regs, std::vector<sw_singlestep_nextpc_t> &swSingleStepNextPCs)
{
    // Note, Linux kernel could offers some helpers/intrisics in a high page that we can't read (and write).
    // For BL and BLX move to address of following instruction, in case tail called functions return to the address in LR.

    std::uintptr_t currentPC = std::uintptr_t(regs.uregs[REG_PC]);

    for (auto &entry : swSingleStepNextPCs)
    {
        if (entry.addr <= 0xffff0000)
            continue;

        bool isBLorBLX = false;
        std::uintptr_t incrPC = 0;
        std::uint16_t inst1 = 0;
        if (!GetDataFromMemory<std::uint16_t>(pid, currentPC, inst1))
            return false;

        if (GetBits(inst1, 8, 15) == 0x47 && GetBit(inst1, 7)) // BLX register
        {
            isBLorBLX = true;
            incrPC = 2;
        }
        else if (ThumbInsnructionSize(inst1) == 4) // 32-bit instruction
        {
            std::uint16_t inst2 = 0;
            if (!GetDataFromMemory<std::uint16_t>(pid, currentPC + 2, inst2))
                return false;

            if ((inst1 & 0xf800) == 0xf000 && GetBits(inst2, 14, 15) == 0x3) // BL <label> or BLX <label>
            {
                isBLorBLX = true;
                incrPC = 4;
            }
        }

        entry.isThumb = true;

        if (isBLorBLX)
            entry.addr = currentPC + incrPC;
        else
            entry.addr = std::uintptr_t(regs.uregs[REG_LR]);
    }

    return true;
}

// Get next possible addresses for Thumb instruction subset.
static bool GetThumbCodeNextPCs(pid_t pid, const user_regs_struct &regs, std::vector<sw_singlestep_nextpc_t> &swSingleStepNextPCs)
{
    std::uintptr_t currentPC = std::uintptr_t(regs.uregs[REG_PC]);
    std::uint32_t currentData32 = 0;
    if (!GetDataFromMemory<std::uint32_t>(pid, currentPC, currentData32))
        return false;

    static std::vector<std::function<bool(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t, std::uint16_t, std::uintptr_t&, bool&)>> thumb16Operations
    {
        Thumb16_Default, // 0x0
        Thumb16_Default, // 0x1
        Thumb16_Default, // 0x2
        Thumb16_Default, // 0x3
        Thumb16_BranchExchangeAndDataProcessing, // 0x4 - Branch Exchange, Data-processing register
        Thumb16_Default, // 0x5
        Thumb16_Default, // 0x6
        Thumb16_Default, // 0x7
        Thumb16_Default, // 0x8
        Thumb16_Default, // 0x9
        Thumb16_Default, // 0xa
        Thumb16_Miscellaneous, // 0xb - POP {reglist, pc}, CBZ or CBNZ (Compare and Branch on Zero, Compare and Branch on Non-Zero)
        Thumb16_Default, // 0xc
        Thumb16_ConditionalBranch, // 0xd - Conditional branch
        Thumb16_UnconditionalBranch // 0xe - Unconditional branch
    };

    struct thumb32entry
    {
        std::uint32_t mask;
        std::uint32_t opcode;
        std::function<bool(pid_t, const user_regs_struct&, std::uintptr_t, std::uintptr_t, std::uint16_t, std::uint16_t, std::uintptr_t&, bool&)> func;
    };
    static std::vector<thumb32entry> thumb32Operations
    {
        // mask/opcode - instr2(16bit):instr1(16bit)
        {0x8000f800, 0x8000f000, Thumb32_BranchesMiscControl}, // Branches, miscellaneous control instructions
        {0x2000ffd0, 0x0000e910, Thumb32_LDM}, // LDMDB
        {0x2000ffd0, 0x0000e890, Thumb32_LDM}, // LDMIA
        {0xffffffd0, 0xc000e990, Thumb32_RFE}, // RFEDB
        {0xffffffd0, 0xc000e810, Thumb32_RFE}, // RFEIA
        {0xf0f0ffef, 0x0000ea4f, Thumb32_MOV}, // MOV{S}
        {0xfff0fff0, 0xf000e8d0, Thumb32_TBB}, // TBB
        {0xfff0fff0, 0xf010e8d0, Thumb32_TBH}, // TBH
        {0xf000ff70, 0xf000f850, Thumb32_LDR}, // LDR, where Rm is PC
    };

    std::uintptr_t currentPS = std::uintptr_t(regs.uregs[REG_CPSR]);
    if (!GetThumbConditionalBlockNextPCs(pid, currentPS, currentPC, ((std::uint16_t*)&currentData32)[0], swSingleStepNextPCs))
        return false;
    else if (swSingleStepNextPCs.empty())
    {
        std::uintptr_t nextPC = currentPC + 2; // default PC changes for thumb16
        bool switchToThumbCode = true;

        if (!IsThumbOpcode32Bits(currentData32)) // 16-bit instruction
        {
            unsigned op = GetBits(currentData32, 12, 15);
            if (op < thumb16Operations.size())
            {
                if (!thumb16Operations[op](pid, regs, currentPS, currentPC, ((std::uint16_t*)&currentData32)[0], nextPC, switchToThumbCode))
                    return false;
            }
        }
        else // 32-bit instruction
        {
            nextPC = currentPC + 4; // default PC changes for thumb32

            for (auto &entry : thumb32Operations)
            {
                if ((currentData32 & entry.mask) == entry.opcode)
                {
                    if (!entry.func(pid, regs, currentPS, currentPC, ((std::uint16_t*)&currentData32)[0], ((std::uint16_t*)&currentData32)[1], nextPC, switchToThumbCode))
                        return false;

                    break;
                }
            }
        }

        swSingleStepNextPCs.emplace_back(nextPC, switchToThumbCode);
    }

    return FixThumbCodeNextPCs(pid, regs, swSingleStepNextPCs);
}

bool ARM32_DoSoftwareSingleStep(pid_t pid, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints)
{
    user_regs_struct regs;
    iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(user_regs_struct);
    if (async_ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &iov) == -1)
    {
        LOGW("Ptrace getregset error: %s\n", strerror(errno));
        return false;
    }

    std::vector<sw_singlestep_nextpc_t> swSingleStepNextPCs;

    if (!IsExecutingThumb(regs))
    {
        // TODO care about atomic sequence of instructions beginning with LDREX{,B,H,D} and ending with STREX{,B,H,D}.
        if (!GetArmCodeNextPCs(pid, regs, swSingleStepNextPCs))
            return false;
    }
    else
    {
        // TODO care about atomic sequence of instructions beginning with LDREX{,B,H,D} and ending with STREX{,B,H,D}.
        if (!GetThumbCodeNextPCs(pid, regs, swSingleStepNextPCs))
            return false;
    }

    if (swSingleStepNextPCs.empty())
        return false;

    for (auto &entry : swSingleStepNextPCs)
    {
        errno = 0;  // Since the value returned by a successful PTRACE_PEEK* request may be -1, the caller must clear errno before the call,
                    // and then check it afterward to determine whether or not an error occurred.
        word_t nextPCData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)entry.addr, nullptr);
        if (errno != 0)
        {
            LOGE("Ptrace peekdata error: %s", strerror(errno));
            return false;
        }

        word_t dataWithBrk = EncodeBrkOpcode(nextPCData, entry.isThumb);

        if (async_ptrace(PTRACE_POKEDATA, pid, (void*)entry.addr, (void*)dataWithBrk) == -1)
        {
            LOGE("Ptrace pokedata error: %s", strerror(errno));
            return false;
        }

        swSingleStepBreakpoints.emplace_back(entry.addr, nextPCData);
    }

    return true;
};

} // namespace InteropDebugging
} // namespace netcoredbg
