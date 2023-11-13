// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <sstream>
#include "debugger/frames.h"
#include "metadata/typeprinter.h"
#include "utils/platform.h"
#include "utils/logger.h"
#include "utils/torelease.h"
#ifdef INTEROP_DEBUGGING
#include "debugger/interop_debugging.h"
#if DEBUGGER_UNIX_ARM
#include "debugger/interop_ptrace_helpers.h"
#include <sys/uio.h> // iovec
#include <elf.h> // NT_PRSTATUS
#endif
#endif // INTEROP_DEBUGGING


namespace netcoredbg
{

#ifdef INTEROP_DEBUGGING
namespace
{
    std::mutex g_mutexInteropDebugger;
    InteropDebugging::InteropDebugger *g_pInteropDebugger = nullptr;
} // unnamed namespace
#endif // INTEROP_DEBUGGING

static std::uintptr_t GetIP(CONTEXT *context)
{
#if defined(_TARGET_AMD64_)
    return (std::uintptr_t)context->Rip;
#elif defined(_TARGET_X86_)
    return (std::uintptr_t)context->Eip;
#elif defined(_TARGET_ARM_)
    return (std::uintptr_t)context->Pc;
#elif defined(_TARGET_ARM64_)
    return (std::uintptr_t)context->Pc;
#elif defined(_TARGET_RISCV64_)
    return (std::uintptr_t)context->Pc;
#else
#error "Unsupported platform"
#endif
}

static std::uintptr_t GetSP(CONTEXT *context)
{
#if defined(_TARGET_AMD64_)
    return (std::uintptr_t)context->Rsp;
#elif defined(_TARGET_X86_)
    return (std::uintptr_t)context->Esp;
#elif defined(_TARGET_ARM_)
    return (std::uintptr_t)context->Sp;
#elif defined(_TARGET_ARM64_)
    return (std::uintptr_t)context->Sp;
#elif defined(_TARGET_RISCV64_)
    return (std::uintptr_t)context->Sp;
#else
#error "Unsupported platform"
#endif
}

static std::uintptr_t GetFP(CONTEXT *context)
{
#if defined(_TARGET_AMD64_)
    return (std::uintptr_t)context->Rbp;
#elif defined(_TARGET_X86_)
    return (std::uintptr_t)context->Ebp;
#elif defined(_TARGET_ARM_)
    return (std::uintptr_t)context->R11;
#elif defined(_TARGET_ARM64_)
    return (std::uintptr_t)context->Fp;
#elif defined(_TARGET_RISCV64_)
    return (std::uintptr_t)context->Fp;
#else
#error "Unsupported platform"
#endif
}

static void SetFP(CONTEXT *context, std::uintptr_t value)
{
#if defined(_TARGET_AMD64_)
    context->Rbp = value;
#elif defined(_TARGET_X86_)
    context->Ebp = value;
#elif defined(_TARGET_ARM_)
    context->R11 = value;
#elif defined(_TARGET_ARM64_)
    context->Fp = value;
#elif defined(_TARGET_RISCV64_)
    context->Fp = value;
#else
#error "Unsupported platform"
#endif
}

#ifdef INTEROP_DEBUGGING
#if DEBUGGER_UNIX_ARM
static HRESULT EmptyContextForFrame(WalkFramesCallback cb)
{
    NativeFrame result;
    result.unknownFrameAddr = true;
    result.procName = "[Native Frame(s), unwind failed - CoreCLR don't provide registers context]";
    return cb(FrameNative, result.addr, nullptr, &result);
}

static HRESULT EmptyContextForTopFrame(ICorDebugThread *pThread, WalkFramesCallback cb)
{
    std::lock_guard<std::mutex> lock(g_mutexInteropDebugger);

    if (g_pInteropDebugger == nullptr)
        return EmptyContextForFrame(cb);

    DWORD threadId = 0;
    StackFrame frame;
    if (SUCCEEDED(pThread->GetID(&threadId)))
    {
        user_regs_struct regs;
        iovec iov;
        iov.iov_base = &regs;
        iov.iov_len = sizeof(user_regs_struct);
        if (InteropDebugging::async_ptrace(PTRACE_GETREGSET, threadId, (void*)NT_PRSTATUS, &iov) == -1)
        {
            LOGW("Ptrace getregset error: %s\n", strerror(errno));
        }
        else
        {
            constexpr int REG_PC = 15;
            if (SUCCEEDED(g_pInteropDebugger->GetFrameForAddr(regs.uregs[REG_PC], frame)))
            {
                NativeFrame result;
                result.addr = regs.uregs[REG_PC];
                result.libName = frame.moduleOrLibName;
                result.procName = frame.methodName;
                result.fullSourcePath = frame.source.path;
                result.lineNum = frame.line;
                HRESULT Status;
                IfFailRet(cb(FrameNative, result.addr, nullptr, &result));
            }
        }
    }

    return EmptyContextForFrame(cb);
}
#endif // DEBUGGER_UNIX_ARM
#endif // INTEROP_DEBUGGING

static HRESULT UnwindNativeFrames(ICorDebugThread *pThread, bool firstFrame, CONTEXT *pStartContext, CONTEXT *pEndContext, WalkFramesCallback cb)
{
#ifdef INTEROP_DEBUGGING
    std::lock_guard<std::mutex> lock(g_mutexInteropDebugger);

    if (g_pInteropDebugger == nullptr)
        return S_OK; // In case debug session without interop, we merge "[CoreCLR Native Frame]" and "user's native frame" into "[Native Frames]".

    DWORD threadId = 0;
    if (FAILED(pThread->GetID(&threadId)))
    {
        NativeFrame result;
        result.addr = pStartContext ? GetIP(pStartContext) : 0;
        result.unknownFrameAddr = !result.addr;
        result.procName = "[Native Frame(s)]";
        return cb(FrameNative, result.addr, nullptr, &result);
    }

    std::uintptr_t endAddr = pEndContext ? GetIP(pEndContext) : 0;
    return g_pInteropDebugger->UnwindNativeFrames(threadId, firstFrame, endAddr, pStartContext, [&](NativeFrame &nativeFrame)
    {
        return cb(FrameNative, nativeFrame.addr, nullptr, &nativeFrame);
    });
#else
    // In case not interop build we merge "CoreCLR native frame" and "user's native frame" into "[Native Frames]".
    return S_OK;
#endif // INTEROP_DEBUGGING
}

#ifdef INTEROP_DEBUGGING
static HRESULT UnwindInlinedTopNativeFrames(ICorDebugThread *pThread, ICorDebugFunction *pFunction, CONTEXT &currentCtx, WalkFramesCallback cb)
{
    ToRelease<ICorDebugFunction2> iCorFunc2;
    BOOL bJustMyCode;
    if (SUCCEEDED(pFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID *)&iCorFunc2)) &&
        SUCCEEDED(iCorFunc2->GetJMCStatus(&bJustMyCode)) &&
        // Check for optimized code. In case of optimized code, JMC status can't be set to TRUE.
        // https://github.com/dotnet/runtime/blob/main/src/coreclr/debug/ee/debugger.cpp#L11257-L11260
        SUCCEEDED(iCorFunc2->SetJMCStatus(TRUE)))
    {
        // Revert back JMC status if need.
        if (bJustMyCode != TRUE)
            iCorFunc2->SetJMCStatus(bJustMyCode);

        return S_OK; // not optimized code for sure, ignore native top frames.
    }

    // Prevent native top frame unwinding in case thread stopped by managed exception.
    ToRelease<ICorDebugValue> iCorExceptionValue;
    if (SUCCEEDED(pThread->GetCurrentException(&iCorExceptionValue)) && iCorExceptionValue != nullptr)
        return S_OK;

    std::lock_guard<std::mutex> lock(g_mutexInteropDebugger);

    DWORD threadId = 0;
    if (g_pInteropDebugger == nullptr || FAILED(pThread->GetID(&threadId)))
        return S_OK;

    HRESULT Status;
    static const bool firstFrame = true;
#if DEBUGGER_UNIX_ARM
    // Linux arm32 CoreCLR have issue:
    // - ICorDebugStackWalk::GetContext return empty registers context for all frames;
    if (GetIP(&currentCtx) == 0)
        IfFailRet(EmptyContextForTopFrame(pThread, cb));
    else
#endif // DEBUGGER_UNIX_ARM
    IfFailRet(g_pInteropDebugger->UnwindNativeFrames(threadId, firstFrame, GetIP(&currentCtx), nullptr, [&](NativeFrame &nativeFrame)
    {
        return cb(FrameNative, nativeFrame.addr, nullptr, &nativeFrame);
    }));

    return S_OK;
}
#endif // INTEROP_DEBUGGING

// From https://github.com/SymbolSource/Microsoft.Samples.Debugging/blob/master/src/debugger/mdbgeng/FrameFactory.cs
HRESULT WalkFrames(ICorDebugThread *pThread, WalkFramesCallback cb)
{
    HRESULT Status;

    ToRelease<ICorDebugThread3> iCorThread3;
    IfFailRet(pThread->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &iCorThread3));
    ToRelease<ICorDebugStackWalk> iCorStackWalk;
    IfFailRet(iCorThread3->CreateStackWalk(&iCorStackWalk));

    static const ULONG32 ctxFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    CONTEXT ctxUnmanagedChain;
    bool ctxUnmanagedChainValid = false;
    CONTEXT currentCtx;
    ULONG32 contextSize;
    memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
    memset(&currentCtx, 0, sizeof(CONTEXT));

    // TODO ICorDebugInternalFrame support for more info about CoreCLR related internal routine and call cb() with `FrameCLRInternal`
    // ICorDebugThread3::GetActiveInternalFrames
    // https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/debugging/icordebugthread3-getactiveinternalframes-method

    int level = -1;
    static const bool firstFrame = true;

    for (Status = S_OK; ; Status = iCorStackWalk->Next())
    {
        if (Status == CORDBG_S_AT_END_OF_STACK)
            break;

        level++;

        IfFailRet(Status);

        ToRelease<ICorDebugFrame> iCorFrame;
        IfFailRet(iCorStackWalk->GetFrame(&iCorFrame));
        if (Status == S_FALSE) // S_FALSE - The current frame is a native stack frame.
        {
            // We've hit a native frame, we need to store the CONTEXT
            memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
            IfFailRet(iCorStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &ctxUnmanagedChain));
            ctxUnmanagedChainValid = true;
            continue;
        }

        // At this point (Status == S_OK).
        // Accordingly to CoreCLR sources, S_OK could be with nulled iCorFrame, that must be skipped.
        // Related to `FrameType::kExplicitFrame` in runtime (skipped frame function with no-frame transition represents)
        if (iCorFrame == NULL)
            continue;

        // If we get a RuntimeUnwindableFrame, then the stackwalker is also stopped at a native
        // stack frame, but it's a native stack frame which requires special unwinding help from
        // the runtime. When a debugger gets a RuntimeUnwindableFrame, it should use the runtime
        // to unwind, but it has to do inspection on its own. It can call
        // ICorDebugStackWalk::GetContext() to retrieve the context of the native stack frame.
        ToRelease<ICorDebugRuntimeUnwindableFrame> iCorRuntimeUnwindableFrame;
        if (SUCCEEDED(iCorFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &iCorRuntimeUnwindableFrame)))
            continue;

        // We need to store the CONTEXT when we're at a managed frame.
        memset(&currentCtx, 0, sizeof(CONTEXT));
        IfFailRet(iCorStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &currentCtx));
        // Note, we don't change top managed frame FP in case we don't have SP (for example, registers context related issue)
        // or CoreCLR was able to restore it. This case could happens only with "managed" top frame (`GetFrame()` return `S_OK`),
        // where real top frame is native (for example, optimized managed code with inlined pinvoke or CoreCLR native frame).
        if (level == 0 && GetSP(&currentCtx) != 0 && GetFP(&currentCtx) == 0)
        {
            SetFP(&currentCtx, GetSP(&currentCtx));
            IfFailRet(iCorStackWalk->SetContext(SET_CONTEXT_FLAG_UNWIND_FRAME, sizeof(CONTEXT), (BYTE*) &currentCtx));
        }

        // Check if we have native frames to unwind
        if (ctxUnmanagedChainValid)
        {
#ifdef INTEROP_DEBUGGING
#if DEBUGGER_UNIX_ARM
            // Linux arm32 CoreCLR have issues:
            // - ICorDebugStackWalk::Next have first stack frame "native", ICorDebugStackWalk::GetFrame return S_FALSE;
            // - ICorDebugStackWalk::GetContext return empty registers context for all frames;
            if (GetIP(&ctxUnmanagedChain) == 0 || GetIP(&currentCtx) == 0)
            {
                if (level == 1)
                    IfFailRet(EmptyContextForTopFrame(pThread, cb));
                else
                    IfFailRet(EmptyContextForFrame(cb));
            }
            else
#endif // DEBUGGER_UNIX_ARM
#endif // INTEROP_DEBUGGING
            IfFailRet(UnwindNativeFrames(pThread, !firstFrame, &ctxUnmanagedChain, &currentCtx, cb));
            level++;
            // Clear out the CONTEXT
            memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
            ctxUnmanagedChainValid = false;
        }

        // Return the managed frame
        ToRelease<ICorDebugFunction> iCorFunction;
        if (SUCCEEDED(iCorFrame->GetFunction(&iCorFunction)))
        {
#ifdef INTEROP_DEBUGGING
            // In case of optimized managed code, top frame could be native (optimized code could have inlined pinvoke).
            // Note, breakpoint can't be set in optimized managed code and step can't stop here, since this code is not JMC for sure.
            if (level == 0 && FAILED(Status = UnwindInlinedTopNativeFrames(pThread, iCorFunction.GetPtr(), currentCtx, cb)))
                return Status;
#endif // INTEROP_DEBUGGING

            IfFailRet(cb(FrameCLRManaged, GetIP(&currentCtx), iCorFrame, nullptr));
            continue;
        }

        ToRelease<ICorDebugNativeFrame> iCorNativeFrame;
        if (FAILED(iCorFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &iCorNativeFrame)))
        {
            IfFailRet(cb(FrameUnknown, GetIP(&currentCtx), iCorFrame, nullptr));
            continue;
        }
        // If the first frame is CoreCLR native frame then we might be in a call to unmanaged code.
        // Note, in case start unwinding from native code we get CoreCLR native frame first, not some native frame at the top,
        // since CoreCLR debug API don't track native code execution and don't really "see" native code at the beginning of unwinding.
        if (level == 0)
        {
#ifdef INTEROP_DEBUGGING
#if DEBUGGER_UNIX_ARM
            // Linux arm32 CoreCLR have issue:
            // - ICorDebugStackWalk::GetContext return empty registers context for all frames;
            if (GetIP(&currentCtx) == 0)
                IfFailRet(EmptyContextForTopFrame(pThread, cb));
            else
#endif // DEBUGGER_UNIX_ARM
#endif // INTEROP_DEBUGGING
            IfFailRet(UnwindNativeFrames(pThread, firstFrame, nullptr, &currentCtx, cb));
        }
        IfFailRet(cb(FrameCLRNative, GetIP(&currentCtx), iCorFrame, nullptr));
    }

    // We may have native frames at the end of the stack
    if (ctxUnmanagedChainValid)
    {
        if (level == 0) // in case this is first and last frame - unwind all
            IfFailRet(UnwindNativeFrames(pThread, firstFrame, nullptr, nullptr, cb));
        else
        {
#ifdef INTEROP_DEBUGGING
#if DEBUGGER_UNIX_ARM
            // Linux arm32 CoreCLR have issue:
            // - ICorDebugStackWalk::GetContext return empty registers context for all frames;
            if (GetIP(&ctxUnmanagedChain) == 0)
                IfFailRet(EmptyContextForFrame(cb));
            else
#endif // DEBUGGER_UNIX_ARM
#endif // INTEROP_DEBUGGING
            IfFailRet(UnwindNativeFrames(pThread, !firstFrame, &ctxUnmanagedChain, nullptr, cb));
        }
    }

    return S_OK;
}

HRESULT GetFrameAt(ICorDebugThread *pThread, FrameLevel level, ICorDebugFrame **ppFrame)
{
    // Try get 0 (current active) frame in fast way, if possible.
    if (int(level) == 0 &&
        SUCCEEDED(pThread->GetActiveFrame(ppFrame)) &&
        *ppFrame != nullptr)
        return S_OK;

    int currentFrame = -1;

    WalkFrames(pThread, [&](
        FrameType frameType,
        std::uintptr_t addr,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative)
    {
        currentFrame++;

        if (currentFrame < int(level))
            return S_OK;
        else if (currentFrame > int(level))
            return E_FAIL;

        if (currentFrame != int(level) || frameType != FrameCLRManaged)
            return S_OK;

        pFrame->AddRef();
        *ppFrame = pFrame;
        return E_ABORT; // Fast exit from cycle.
    });

    return *ppFrame != nullptr ? S_OK : E_FAIL;
}

const char *GetInternalTypeName(CorDebugInternalFrameType frameType)
{
    switch(frameType)
    {
        case STUBFRAME_M2U:                  return "Managed to Native Transition";
        case STUBFRAME_U2M:                  return "Native to Managed Transition";
        case STUBFRAME_APPDOMAIN_TRANSITION: return "Appdomain Transition";
        case STUBFRAME_LIGHTWEIGHT_FUNCTION: return "Lightweight function";
        case STUBFRAME_FUNC_EVAL:            return "Func Eval";
        case STUBFRAME_INTERNALCALL:         return "Internal Call";
        case STUBFRAME_CLASS_INIT:           return "Class Init";
        case STUBFRAME_EXCEPTION:            return "Exception";
        case STUBFRAME_SECURITY:             return "Security";
        case STUBFRAME_JIT_COMPILATION:      return "JIT Compilation";
        default:                             return "Unknown";
    }
}

#ifdef INTEROP_DEBUGGING

void InitNativeFramesUnwind(InteropDebugging::InteropDebugger *pInteropDebugger)
{
    g_mutexInteropDebugger.lock();
    g_pInteropDebugger = pInteropDebugger;
    g_mutexInteropDebugger.unlock();
}

void ShutdownNativeFramesUnwind()
{
    g_mutexInteropDebugger.lock();
    g_pInteropDebugger = nullptr;
    g_mutexInteropDebugger.unlock();
}

#endif // INTEROP_DEBUGGING

} // namespace netcoredbg
