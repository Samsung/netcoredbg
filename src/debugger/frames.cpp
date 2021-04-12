// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/frames.h"

#include <sstream>
#include <algorithm>

#include "metadata/typeprinter.h"
#include "platform.h"
#include "debugger/manageddebugger.h"
#include "utils/logger.h"

namespace netcoredbg
{

HRESULT GetThreadsState(ICorDebugController *controller, std::vector<Thread> &threads)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugThreadEnum> pThreads;
    IfFailRet(controller->EnumerateThreads(&pThreads));

    const std::string threadName = "<No name>";
    ICorDebugThread *handle = nullptr;
    ULONG fetched = 0;

    while (SUCCEEDED(Status = pThreads->Next(1, &handle, &fetched)) && fetched == 1)
    {
        ToRelease<ICorDebugThread> pThread(handle);

        DWORD threadId = 0;
        IfFailRet(pThread->GetID(&threadId));

        ToRelease<ICorDebugProcess> pProcess;
        IfFailRet(pThread->GetProcess(&pProcess));

        BOOL running = FALSE;
        IfFailRet(pProcess->IsRunning(&running));

        // Baground threads also included. GetUserState() not available for running thread.
        threads.emplace_back(ThreadId{threadId}, threadName, running);

        fetched = 0;
        handle = nullptr;
    }

    return S_OK;
}

uint64_t GetFrameAddr(ICorDebugFrame *pFrame)
{
    CORDB_ADDRESS startAddr = 0;
    CORDB_ADDRESS endAddr = 0;
    pFrame->GetStackRange(&startAddr, &endAddr);
    return startAddr;
}

static uint64_t GetSP(CONTEXT *context)
{
#if defined(_TARGET_AMD64_)
    return (uint64_t)(size_t)context->Rsp;
#elif defined(_TARGET_X86_)
    return (uint64_t)(size_t)context->Esp;
#elif defined(_TARGET_ARM_)
    return (uint64_t)(size_t)context->Sp;
#elif defined(_TARGET_ARM64_)
    return (uint64_t)(size_t)context->Sp;
#elif
#error "Unsupported platform"
#endif
}

HRESULT UnwindNativeFrames(ICorDebugThread *pThread, uint64_t startValue, uint64_t endValue, std::vector<NativeFrame> &frames)
{
    return S_OK;
}

HRESULT StitchInternalFrames(
    ICorDebugThread *pThread,
    const std::vector< ToRelease<ICorDebugFrame> > &internalFrameCache,
    const std::vector<NativeFrame> &nativeFrames,
    WalkFramesCallback cb)
{
    HRESULT Status;

    struct FrameIndex {
        size_t index;
        bool internal;
        uint64_t addr;

        FrameIndex(size_t ind, const ToRelease<ICorDebugFrame> &frame)
            : index(ind), internal(true), addr(GetFrameAddr(frame.GetPtr())) {}
        FrameIndex(size_t ind, const NativeFrame &frame)
            : index(ind), internal(false), addr(frame.addr) {}
    };

    std::vector<FrameIndex> frames;

    for (size_t i = 0; i < nativeFrames.size(); i++) frames.emplace_back(i, nativeFrames[i]);
    for (size_t i = 0; i < internalFrameCache.size(); i++) frames.emplace_back(i, internalFrameCache[i]);

    std::sort(frames.begin( ), frames.end(), [](const FrameIndex& lhs, const FrameIndex& rhs)
    {
        return lhs.addr < rhs.addr;
    });

    for (auto &fr : frames)
    {
        if (fr.internal)
            IfFailRet(cb(FrameCLRInternal, internalFrameCache[fr.index].GetPtr(), nullptr, nullptr));
        else
            IfFailRet(cb(FrameNative, nullptr, const_cast<NativeFrame*>(&nativeFrames[fr.index]), nullptr));
    }

    return S_OK;
}

// From https://github.com/SymbolSource/Microsoft.Samples.Debugging/blob/master/src/debugger/mdbgeng/FrameFactory.cs
HRESULT WalkFrames(ICorDebugThread *pThread, WalkFramesCallback cb)
{
    HRESULT Status;

    ToRelease<ICorDebugThread3> pThread3;
    ToRelease<ICorDebugStackWalk> pStackWalk;

    IfFailRet(pThread->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &pThread3));
    IfFailRet(pThread3->CreateStackWalk(&pStackWalk));

    std::vector< ToRelease<ICorDebugFrame> > iFrameCache;
    std::vector<NativeFrame> nFrames;

    static const ULONG32 ctxFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    CONTEXT ctxUnmanagedChain;
    bool ctxUnmanagedChainValid = false;
    CONTEXT currentCtx;
    bool currentCtxValid = false;
    memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
    memset(&currentCtx, 0, sizeof(CONTEXT));

    int level = -1;

    for (Status = S_OK; ; Status = pStackWalk->Next())
    {
        level++;

        if (Status == CORDBG_S_AT_END_OF_STACK)
            break;

        IfFailRet(Status);

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pStackWalk->GetFrame(&pFrame));
        if (Status == S_FALSE) // S_FALSE - The current frame is a native stack frame.
        {
            // We've hit a native frame, we need to store the CONTEXT
            memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
            ULONG32 contextSize;
            IfFailRet(pStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &ctxUnmanagedChain));
            ctxUnmanagedChainValid = true;
            // We need to invalidate the currentCtx since it won't be valid on the next loop iteration
            memset(&currentCtx, 0, sizeof(CONTEXT));
            currentCtxValid = false;
            continue;
        }

        // At this point (Status == S_OK).
        // Accordingly to CoreCLR sources, S_OK could be with nulled pFrame, that must be skipped.
        if (pFrame == NULL)
            continue;

        // If we get a RuntimeUnwindableFrame, then the stackwalker is stopped at a native
        // stack frame, but requires special unwinding help from the runtime.
        ToRelease<ICorDebugRuntimeUnwindableFrame> pRuntimeUnwindableFrame;
        Status = pFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &pRuntimeUnwindableFrame);
        if (SUCCEEDED(Status))
        {
            continue;
        }

        // We need to check for an internal frame.
        // If the internal frame happens to come after the last managed frame, any call to GetContext() will assert
        ToRelease<ICorDebugInternalFrame> pInternalFrame;
        if (FAILED(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame)))
        {
            // We need to store the CONTEXT when we're at a managed frame.
            // If there's an internal frame after this, then we'll use this CONTEXT
            memset(&currentCtx, 0, sizeof(CONTEXT));
            ULONG32 contextSize;
            IfFailRet(pStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &currentCtx));
            currentCtxValid = true;
        }
        else if (ctxUnmanagedChainValid)
        {
            // We need to check if this internal frame could have been sandwiched between native frames,
            // this will be the case if ctxUnmanagedChain is valid

            // We need to store ALL internal frames until we hit the next managed frame
            iFrameCache.emplace_back(std::move(pFrame));
            continue;
        }
        // Else we'll use the 'stored' currentCtx if we're at an InternalFrame

        uint64_t pEndVal = std::numeric_limits<uint64_t>::max();
        if (currentCtxValid)
        {
            pEndVal = GetSP(&currentCtx);
        }

        // Check if we have native frames to unwind
        if (ctxUnmanagedChainValid)
            IfFailRet(UnwindNativeFrames(pThread, GetSP(&ctxUnmanagedChain), pEndVal, nFrames));
        IfFailRet(StitchInternalFrames(pThread, iFrameCache, nFrames, cb));

        // Clear out the CONTEXT and native frame cache
        memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
        ctxUnmanagedChainValid = false;
        nFrames.clear();
        iFrameCache.clear();

        // Return the managed frame
        ToRelease<ICorDebugFunction> pFunction;
        Status = pFrame->GetFunction(&pFunction);
        if (SUCCEEDED(Status))
        {
            IfFailRet(cb(FrameCLRManaged, pFrame, nullptr, pFunction));
            continue;
        }
        // If we cannot get managed function then return internal or native frame
        FrameType frameType = pInternalFrame ? FrameCLRInternal : FrameCLRNative;

        ToRelease<ICorDebugNativeFrame> pNativeFrame;
        HRESULT hrNativeFrame = pFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &pNativeFrame);
        if (FAILED(hrNativeFrame))
        {
            IfFailRet(cb(FrameUnknown, pFrame, nullptr, nullptr));
            continue;
        }
        // If the first frame is either internal or native then we might be in a call to unmanaged code
        if (level == 0)
        {
            IfFailRet(UnwindNativeFrames(pThread, 0, GetFrameAddr(pFrame), nFrames));
            IfFailRet(StitchInternalFrames(pThread, {}, nFrames, cb));
            nFrames.clear();
        }
        IfFailRet(cb(frameType, pFrame, nullptr, nullptr));
    }

    // We may have native frames at the end of the stack
    uint64_t pEndVal = std::numeric_limits<uint64_t>::max();
    if (ctxUnmanagedChainValid)
        IfFailRet(UnwindNativeFrames(pThread, GetSP(&ctxUnmanagedChain), pEndVal, nFrames));
    IfFailRet(StitchInternalFrames(pThread, iFrameCache, nFrames, cb));

    // After stitching frames they should be cleared like this:
    // nFrames.clear();
    // memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
    // ctxUnmanagedChainValid = false;

    return S_OK;
}

HRESULT GetFrameAt(ICorDebugThread *pThread, FrameLevel level, ICorDebugFrame **ppFrame)
{
    ToRelease<ICorDebugFrame> result;

    int currentFrame = -1;

    HRESULT Status = WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < int(level))
            return S_OK;
        else if (currentFrame > int(level))
            return E_FAIL;

        if (currentFrame == level && frameType == FrameCLRManaged)
        {
            pFrame->AddRef();
            result = pFrame;
        }
        return E_FAIL;
    });

    if (result)
    {
        *ppFrame = result.Detach();
        return S_OK;
    }

    return Status;
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

} // namespace netcoredbg
