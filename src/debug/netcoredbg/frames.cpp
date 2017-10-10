// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include <sstream>
#include <vector>
#include <list>
#include <functional>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <algorithm>

#include "typeprinter.h"
#include "platform.h"
#include "debugger.h"
#include "modules.h"


HRESULT PrintThread(ICorDebugThread *pThread, std::string &output)
{
    HRESULT Status = S_OK;

    std::stringstream ss;

    DWORD threadId = 0;
    IfFailRet(pThread->GetID(&threadId));

    ToRelease<ICorDebugProcess> pProcess;
    IfFailRet(pThread->GetProcess(&pProcess));
    BOOL running = FALSE;
    IfFailRet(pProcess->IsRunning(&running));

    ss << "{id=\"" << threadId
       << "\",name=\"<No name>\",state=\""
       << (running ? "running" : "stopped") << "\"}";

    output = ss.str();

    return S_OK;
}

HRESULT PrintThreadsState(ICorDebugController *controller, std::string &output)
{
    HRESULT Status = S_OK;

    ToRelease<ICorDebugThreadEnum> pThreads;
    IfFailRet(controller->EnumerateThreads(&pThreads));

    std::stringstream ss;

    ss << "threads=[";

    ICorDebugThread *handle;
    ULONG fetched;
    const char *sep = "";
    while (SUCCEEDED(Status = pThreads->Next(1, &handle, &fetched)) && fetched == 1)
    {
        ToRelease<ICorDebugThread> pThread(handle);

        std::string threadOutput;
        PrintThread(pThread, threadOutput);

        ss << sep << threadOutput;
        sep = ",";
    }

    ss << "]";
    output = ss.str();
    return S_OK;
}

static std::string AddrToString(uint64_t addr)
{
    std::stringstream ss;
    ss << "0x" << std::setw(2 * sizeof(void*)) << std::setfill('0') << std::hex << addr;
    return ss.str();
}

static std::string FrameAddrToString(ICorDebugFrame *pFrame)
{
    CORDB_ADDRESS startAddr = 0;
    CORDB_ADDRESS endAddr = 0;
    pFrame->GetStackRange(&startAddr, &endAddr);
    return AddrToString(startAddr);
}

HRESULT PrintFrameLocation(ICorDebugFrame *pFrame, std::string &output)
{
    HRESULT Status;

    ULONG32 ilOffset;
    Modules::SequencePoint sp;
    bool has_source = false;

    std::stringstream ss;

    if (SUCCEEDED(Modules::GetFrameLocation(pFrame, ilOffset, sp)))
    {
        ss << "file=\"" << GetFileName(sp.document) << "\","
           << "fullname=\"" << Debugger::EscapeMIValue(sp.document) << "\","
           << "line=\"" << sp.startLine << "\","
           << "col=\"" << sp.startColumn << "\","
           << "end-line=\"" << sp.endLine << "\","
           << "end-col=\"" << sp.endColumn << "\",";
        has_source = true;
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

    std::string id;
    IfFailRet(Modules::GetModuleId(pModule, id));

    ss << "clr-addr={module-id=\"{" << id << "}\","
       << "method-token=\"0x" << std::setw(8) << std::setfill('0') << std::hex << methodToken << "\","
       << "il-offset=\"" << std::dec << ilOffset << "\",native-offset=\"" << nOffset << "\"},";

    std::string methodName;
    TypePrinter::GetMethodName(pFrame, methodName);
    ss << "func=\"" << methodName << "\",";
    ss << "addr=\"" << FrameAddrToString(pFrame) << "\"";

    output = ss.str();

    return has_source ? S_OK : S_FALSE;
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
            // we've hit a native frame, we need to store the CONTEXT
            memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
            ULONG32 contextSize;
            IfFailRet(pStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &ctxUnmanagedChain));
            ctxUnmanagedChainValid = true;
            // we need to invalidate the currentCtx since it won't be valid on the next loop iteration
            memset(&currentCtx, 0, sizeof(CONTEXT));
            currentCtxValid = false;
            continue;
        }

        // If we get a RuntimeUnwindableFrame, then the stackwalker is also stopped at a native
        // stack frame, but it's a native stack frame which requires special unwinding help from
        // the runtime. When a debugger gets a RuntimeUnwindableFrame, it should use the runtime
        // to unwind, but it has to do inspection on its own. It can call
        // ICorDebugStackWalk::GetContext() to retrieve the context of the native stack frame.
        ToRelease<ICorDebugRuntimeUnwindableFrame> pRuntimeUnwindableFrame;
        Status = pFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &pRuntimeUnwindableFrame);
        if (SUCCEEDED(Status))
        {
            continue;
        }

        // check for an internal frame...if the internal frame happens to come after the last
        // managed frame, any call to GetContext() will assert
        ToRelease<ICorDebugInternalFrame> pInternalFrame;
        if (FAILED(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame)))
        {
            // we need to store the CONTEXT when we're at a managed frame, if there's an internal frame
            // after this, then we'll need this CONTEXT
            memset(&currentCtx, 0, sizeof(CONTEXT));
            ULONG32 contextSize;
            IfFailRet(pStackWalk->GetContext(ctxFlags, sizeof(CONTEXT), &contextSize, (BYTE*) &currentCtx));
            currentCtxValid = true;
        }
        else if (ctxUnmanagedChainValid)
        {
            // we need to check to see if this internal frame could have been sandwiched between
            // native frames, this will be the case if ctxUnmanagedChain is not null

            // we need to store ALL internal frames until we hit the next managed frame
            iFrameCache.emplace_back(std::move(pFrame));
            continue;
        }
        // else we'll use the 'stored' currentCtx if we're at an InternalFrame

        uint64_t pEndVal = std::numeric_limits<uint64_t>::max();
        if (currentCtxValid)
        {
            pEndVal = GetSP(&currentCtx);
        }

        // check to see if we have native frames to unwind
        std::vector<NativeFrame> nFrames;
        if (ctxUnmanagedChainValid)
            IfFailRet(UnwindNativeFrames(pThread, GetSP(&ctxUnmanagedChain), pEndVal, nFrames));
        IfFailRet(StitchInternalFrames(pThread, iFrameCache, nFrames, cb));

        // clear out the CONTEXT and native frame cache
        memset(&ctxUnmanagedChain, 0, sizeof(CONTEXT));
        ctxUnmanagedChainValid = false;
        nFrames.clear();
        iFrameCache.clear();

        // return the managed frame
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

    // we may have native frames at the end of the stack
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

HRESULT PrintFrames(ICorDebugThread *pThread, std::string &output, int lowFrame = 0, int highFrame = INT_MAX)
{
    HRESULT Status;
    std::stringstream ss;

    int currentFrame = -1;

    ss << "stack=[";
    const char *sep = "";

    IfFailRet(WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        NativeFrame *pNative,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < lowFrame)
            return S_OK;
        if (currentFrame > highFrame)
            return S_OK; // Todo implement fast break mechanism

        ss << sep;
        sep = ",";

        switch(frameType)
        {
            case FrameUnknown:
                ss << "frame={level=\"" << currentFrame << "\",func=\"?\"}";
                break;
            case FrameNative:
                ss << "frame={level=\"" << currentFrame << "\",func=\"" << pNative->symbol << "\","
                   << "file=\"" << pNative->file << "\","
                   << "line=\"" << pNative->linenum << "\","
                   << "addr=\"" << AddrToString(pNative->addr) << "\"}";
                break;
            case FrameCLRNative:
                ss << "frame={level=\"" << currentFrame << "\",func=\"[Native Frame]\","
                   << "addr=\"" << FrameAddrToString(pFrame) << "\"}";
                break;
            case FrameCLRInternal:
                {
                    ToRelease<ICorDebugInternalFrame> pInternalFrame;
                    IfFailRet(pFrame->QueryInterface(IID_ICorDebugInternalFrame, (LPVOID*) &pInternalFrame));
                    CorDebugInternalFrameType frameType;
                    IfFailRet(pInternalFrame->GetFrameType(&frameType));
                    ss << "frame={level=\"" << currentFrame << "\",func=\"[" << GetInternalTypeName(frameType) << "]\","
                       << "addr=\"" << FrameAddrToString(pFrame) << "\"}";
                }
                break;
            case FrameCLRManaged:
                {
                    std::string frameLocation;
                    PrintFrameLocation(pFrame, frameLocation);

                    ss << "frame={level=\"" << currentFrame << "\"";
                    if (!frameLocation.empty())
                        ss << "," << frameLocation;
                    ss << "}";
                }
                break;
        }
        return S_OK;
    }));

    ss << "]";

    output = ss.str();

    return S_OK;
}
