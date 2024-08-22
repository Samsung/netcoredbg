// Copyright (c) 2024 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/interop_riscv64_singlestep_helpers.h"
#include "debugger/interop_brk_helpers.h"

#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <climits>
#include "utils/logger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

inline uint64_t SignExtend(uint64_t value, unsigned int signBit)
{
    assert(signBit < 64);

    if (signBit == 63)
      return value;

    uint64_t sign = value & (1ull << signBit);

    if (sign)
        return value | (~0ull << signBit);

    return value;
}

inline uint64_t BitExtract(uint64_t value, unsigned int highBit, unsigned int lowBit, bool signExtend = false)
{
    assert((highBit < 64) && (lowBit < 64) && (highBit >= lowBit));
    uint64_t extractedValue = (value >> lowBit) & ((1ull << ((highBit - lowBit) + 1)) - 1);
    return signExtend ? SignExtend(extractedValue, highBit - lowBit) : extractedValue;
}

inline uint64_t GetReg(const user_regs_struct &regs, uint64_t reg)
{
    assert(reg <= 31);

    if (reg == 0) // user_regs_struct don't store X0, index 0 belong to PC.
        return 0;

    return (&regs.pc)[reg];
}

static void Get16BitCodeNextPC(pid_t pid, const user_regs_struct &regs, const uint16_t opcode, std::uintptr_t &nextPC)
{
    std::uintptr_t currentPC = std::uintptr_t(regs.pc);
    nextPC = currentPC + 2; // default PC changes

    if ((opcode & 0xE003) == 0xA001) // C.J (Note, C.JAL is RV32 only)
    {
        // CJ type immediate
        ///      |15 14 13|12                  2|1 0|
        // imm             11|4|9:8|10|6|7|3:1|5
        uint64_t imm = SignExtend((BitExtract(opcode, 5, 3) << 1) | (BitExtract(opcode, 11, 11) << 4) | (BitExtract(opcode, 2, 2) << 5) |
                                  (BitExtract(opcode, 7, 7) << 6) | (BitExtract(opcode, 6, 6) << 7) | (BitExtract(opcode, 10, 9) << 8) |
                                  (BitExtract(opcode, 8, 8) << 10) | (BitExtract(opcode, 12, 12) << 11), 11);
        nextPC = currentPC + imm;
    }
    else if ((opcode & 0xE07F) == 0x8002) // C.JR, C.JALR (Note, C.JR and C.JALR vary by 1 bit and this condition will be true only for them)
    {
        uint64_t Rs1 = BitExtract(opcode, 11, 7);
        if (Rs1 != 0)
        {
            uint64_t Rs1Value = GetReg(regs, Rs1);
            nextPC = Rs1Value;
        }
    }
    else if ((opcode & 0xC003) == 0xC001) // C.BEQZ, C.BNEZ (Note, C.BEQZ and C.BNEZ vary by 1 bit and this condition will be true only for them)
    {
        // CB type immediate
        ///      |15 14 13|12 11 10|9 8 7|6       2|1 0|
        // imm              8| 4:3        7:6|2:1|5
        uint64_t imm = SignExtend((BitExtract(opcode, 4, 3) << 1) | (BitExtract(opcode, 11, 10) << 3) | (BitExtract(opcode, 2, 2) << 5) |
                                  (BitExtract(opcode, 6, 5) << 6) | (BitExtract(opcode, 12, 12) << 8), 8);

        uint64_t Rs1 = BitExtract(opcode, 9, 7);
        uint64_t Rs1Value = GetReg(regs, Rs1);

        if (BitExtract(opcode, 13, 13))
        {// C.BNEZ
            if (Rs1Value != 0)
                nextPC = currentPC + imm;
        }
        else
        {// C.BEQZ
            if (Rs1Value == 0)
                nextPC = currentPC + imm;
        }
    }
}

static void Get32BitCodeNextPC(pid_t pid, const user_regs_struct &regs, const uint32_t opcode, std::uintptr_t &nextPC)
{
    std::uintptr_t currentPC = std::uintptr_t(regs.pc);
    nextPC = currentPC + 4; // default PC changes

    if ((opcode & 0x7f) == 0x6f) // JAL
    {
        // J-immediate encodes a signed offset in multiples of 2 bytes
        //      20       | 19                                               1 | 0
        // inst[31]/sign | inst[19:12] | inst[20] | inst[30:25] | inst[24:21] | 0
        uint64_t imm = SignExtend((BitExtract(opcode, 30, 21) << 1) | (BitExtract(opcode, 20, 20) << 11) |
                                  (BitExtract(opcode, 19, 12) << 12) | (BitExtract(opcode, 31, 31) << 20), 20);
        nextPC = currentPC + imm;
    }
    else if ((opcode & 0x707f) == 0x67) // JALR
    {
        // I-immediate
        uint64_t imm = BitExtract(opcode, 31, 20, true);
        uint64_t Rs1 = BitExtract(opcode, 19, 15);
        nextPC = (GetReg(regs, Rs1) + imm) & ~1ull;
    }
    else if (((opcode & 0x707f) == 0x63) ||   // BEQ
             ((opcode & 0x707f) == 0x1063) || // BNE
             ((opcode & 0x707f) == 0x4063) || // BLT
             ((opcode & 0x707f) == 0x5063))   // BGE
    {
        uint64_t Rs1 = BitExtract(opcode, 19, 15);
        uint64_t Rs2 = BitExtract(opcode, 24, 20);

        uint64_t value = GetReg(regs, Rs1);
        int64_t Rs1SValue;
        if (value <= LLONG_MAX)
            Rs1SValue = value;
        else
            Rs1SValue = -(int64_t)(ULLONG_MAX - value) - 1;

        value = GetReg(regs, Rs2);
        int64_t Rs2SValue;
        if (value <= LLONG_MAX)
            Rs2SValue = value;
        else
            Rs2SValue = -(int64_t)(ULLONG_MAX - value) - 1;

        if ((((opcode & 0x707f) == 0x63) && Rs1SValue == Rs2SValue) ||
            (((opcode & 0x707f) == 0x1063) && Rs1SValue != Rs2SValue) ||
            (((opcode & 0x707f) == 0x4063) && Rs1SValue < Rs2SValue) ||
            (((opcode & 0x707f) == 0x5063) && Rs1SValue >= Rs2SValue))
        {
            // B-immediate encodes a signed offset in multiples of 2 bytes
            //       12      | 11                               1 | 0
            // inst[31]/sign | inst[7] | inst[30:25] | inst[11:8] | 0
            uint64_t imm = SignExtend((BitExtract(opcode, 11, 8) << 1) | (BitExtract(opcode, 30, 25) << 5) |
                                      (BitExtract(opcode, 7, 7) << 11) | (BitExtract(opcode, 31, 31) << 12), 12);
            nextPC = currentPC + imm;
        }
    }
    else if (((opcode & 0x707f) == 0x6063) || // BLTU
             ((opcode & 0x707f) == 0x7063))   // BGEU
    {
        uint64_t Rs1 = BitExtract(opcode, 19, 15);
        uint64_t Rs2 = BitExtract(opcode, 24, 20);
        uint64_t Rs1Value = GetReg(regs, Rs1);
        uint64_t Rs2Value = GetReg(regs, Rs2);

        if ((((opcode & 0x707f) == 0x6063) && Rs1Value < Rs2Value) ||
            (((opcode & 0x707f) == 0x7063) && Rs1Value >= Rs2Value))
        {
            // B-immediate encodes a signed offset in multiples of 2 bytes
            //       12      | 11                               1 | 0
            // inst[31]/sign | inst[7] | inst[30:25] | inst[11:8] | 0
            uint64_t imm = SignExtend((BitExtract(opcode, 11, 8) << 1) | (BitExtract(opcode, 30, 25) << 5) |
                                      (BitExtract(opcode, 7, 7) << 11) | (BitExtract(opcode, 31, 31) << 12), 12);
            nextPC = currentPC + imm;
        }
    }
}

bool RISCV64_DoSoftwareSingleStep(pid_t pid, std::vector<sw_singlestep_brk_t> &swSingleStepBreakpoints)
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

    errno = 0;
    word_t currentPCData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)regs.pc, nullptr);
    if (errno != 0)
    {
        LOGE("Ptrace peekdata error: %s", strerror(errno));
        return false;
    }

    std::uintptr_t nextPC;

    if (IsOpcode16Bits(currentPCData))
    {
        // TODO care about atomic sequence of instructions.
        Get16BitCodeNextPC(pid, regs, currentPCData, nextPC);
    }
    else
    {
        // TODO care about atomic sequence of instructions.
        // TODO for ECALL care about SIGRETURN/RT_SIGRETURN syscalls.
        Get32BitCodeNextPC(pid, regs, currentPCData, nextPC);
    }

    errno = 0;  // Since the value returned by a successful PTRACE_PEEK* request may be -1, the caller must clear errno before the call,
                // and then check it afterward to determine whether or not an error occurred.
    word_t nextPCData = async_ptrace(PTRACE_PEEKDATA, pid, (void*)nextPC, nullptr);
    if (errno != 0)
    {
        LOGE("Ptrace peekdata error: %s", strerror(errno));
        return false;
    }

    word_t dataWithBrk = EncodeBrkOpcode(nextPCData, false);

    if (async_ptrace(PTRACE_POKEDATA, pid, (void*)nextPC, (void*)dataWithBrk) == -1)
    {
        LOGE("Ptrace pokedata error: %s", strerror(errno));
        return false;
    }

    swSingleStepBreakpoints.emplace_back(nextPC, nextPCData);

    return true;
}

} // namespace InteropDebugging
} // namespace netcoredbg
