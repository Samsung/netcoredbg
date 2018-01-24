// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <vector>
#include <list>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <algorithm>

#include "typeprinter.h"
#include "platform.h"
#include "manageddebugger.h"
#include "frames.h"


HRESULT GetThreadsState(ICorDebugController *controller, std::vector<Thread> &threads)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugThreadEnum> pThreads;
    IfFailRet(controller->EnumerateThreads(&pThreads));

    ICorDebugThread *handle;
    ULONG fetched;

    while (SUCCEEDED(Status = pThreads->Next(1, &handle, &fetched)) && fetched == 1)
    {
        ToRelease<ICorDebugThread> pThread(handle);

        DWORD threadId = 0;
        IfFailRet(pThread->GetID(&threadId));

        ToRelease<ICorDebugProcess> pProcess;
        IfFailRet(pThread->GetProcess(&pProcess));
        BOOL running = FALSE;
        IfFailRet(pProcess->IsRunning(&running));

        threads.emplace_back(threadId, "<No name>", running);
        std::string threadOutput;
    }

    return S_OK;
}

static uint64_t FrameAddr(ICorDebugFrame *pFrame)
{
    CORDB_ADDRESS startAddr = 0;
    CORDB_ADDRESS endAddr = 0;
    pFrame->GetStackRange(&startAddr, &endAddr);
    return startAddr;
}

HRESULT ManagedDebugger::GetFrameLocation(ICorDebugFrame *pFrame, int threadId, uint32_t level, StackFrame &stackFrame)
{
    HRESULT Status;

    stackFrame = StackFrame(threadId, level, "");

    ULONG32 ilOffset;
    Modules::SequencePoint sp;

    if (SUCCEEDED(m_modules.GetFrameILAndSequencePoint(pFrame, ilOffset, sp)))
    {
        stackFrame.source = Source(sp.document);
        stackFrame.line = sp.startLine;
        stackFrame.column = sp.startColumn;
        stackFrame.endLine = sp.endLine;
        stackFrame.endColumn = sp.endColumn;
    }

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 nOffset = 0;
    ToRelease<ICorDebugNativeFrame> pNativeFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugNativeFrame, (LPVOID*) &pNativeFrame));
    IfFailRet(pNativeFrame->GetIP(&nOffset));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    IfFailRet(Modules::GetModuleId(pModule, stackFrame.moduleId));

    stackFrame.clrAddr.methodToken = methodToken;
    stackFrame.clrAddr.ilOffset = ilOffset;
    stackFrame.clrAddr.nativeOffset = nOffset;

    TypePrinter::GetMethodName(pFrame, stackFrame.name);

    return stackFrame.source.IsNull() ? S_FALSE : S_OK;
}

struct NativeFrame
{
    uint64_t addr;
    std::string symbol;
    std::string file;
    std::string fullname;
    int linenum;
    int tid;
    NativeFrame() : addr(0), linenum(0), tid(0) {}
};

enum FrameType
{
    FrameUnknown,
    FrameNative,
    FrameCLRNative,
    FrameCLRInternal,
    FrameCLRManaged
};

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

static uint64_t GetFrameAddr(ICorDebugFrame *pFrame)
{
    CORDB_ADDRESS startAddr = 0;
    CORDB_ADDRESS endAddr = 0;
    pFrame->GetStackRange(&startAddr, &endAddr);
    return startAddr;
}

typedef std::function<HRESULT(FrameType,ICorDebugFrame*,NativeFrame*,ICorDebugFunction*)> WalkFramesCallback;

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
        if (Status == S_FALSE)
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
        std::vector<NativeFrame> nFrames;
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
        // If the first frame is either internal or native then we might be in a call to unmanged code
        if (level == 0)
        {
            std::vector<NativeFrame> nFrames;
            IfFailRet(UnwindNativeFrames(pThread, 0, GetFrameAddr(pFrame), nFrames));
            IfFailRet(StitchInternalFrames(pThread, {}, nFrames, cb));
        }
        IfFailRet(cb(frameType, pFrame, nullptr, nullptr));
    }

    // We may have native frames at the end of the stack
    uint64_t pEndVal = std::numeric_limits<uint64_t>::max();
    std::vector<NativeFrame> nFrames;
    if (ctxUnmanagedChainValid)
        IfFailRet(UnwindNativeFrames(pThread, GetSP(&ctxUnmanagedChain), pEndVal, nFrames));
    IfFailRet(StitchInternalFrames(pThread, iFrameCache, nFrames, cb));

    nFrames.clear();
    memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
    ctxUnmanagedChainValid = false;

    return S_OK;
}

HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame)
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

        if (currentFrame < level)
            return S_OK;
        else if (currentFrame > level)
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

static const char *GetInternalTypeName(CorDebugInternalFrameType frameType)
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

HRESULT ManagedDebugger::GetStackTrace(ICorDebugThread *pThread, int startFrame, int levels, std::vector<StackFrame> &stackFrames, int &totalFrames)
{
    HRESULT Status;
    std::stringstream ss;

    DWORD threadId = 0;
    pThread->GetID(&threadId);

    int currentFrame = -1;

    IfFailRet(WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < startFrame)
            return S_OK;
        if (levels != 0 && currentFrame >= (startFrame + levels))
            return S_OK;

        switch(frameType)
        {
            case FrameUnknown:
                stackFrames.emplace_back(threadId, currentFrame, "?");
                stackFrames.back().addr = FrameAddr(pFrame);
                break;
            case FrameNative:
                stackFrames.emplace_back(threadId, currentFrame, pNative->symbol);
                stackFrames.back().addr = pNative->addr;
                stackFrames.back().source = Source(pNative->file);
                stackFrames.back().line = pNative->linenum;
                break;
            case FrameCLRNative:
                stackFrames.emplace_back(threadId, currentFrame, "[Native Frame]");
                stackFrames.back().addr = FrameAddr(pFrame);
                break;
            case FrameCLRInternal:
                {
                    ToRelease<ICorDebugInternalFrame> pInternalFrame;
                    IfFailRet(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame));
                    CorDebugInternalFrameType frameType;
                    IfFailRet(pInternalFrame->GetFrameType(&frameType));
                    std::string name = "[";
                    name += GetInternalTypeName(frameType);
                    name += "]";
                    stackFrames.emplace_back(threadId, currentFrame, name);
                    stackFrames.back().addr = FrameAddr(pFrame);
                }
                break;
            case FrameCLRManaged:
                {
                    StackFrame stackFrame;
                    GetFrameLocation(pFrame, threadId, currentFrame, stackFrame);
                    stackFrames.push_back(stackFrame);
                }
                break;
        }
        return S_OK;
    }));

    totalFrames = currentFrame + 1;

    return S_OK;
}
