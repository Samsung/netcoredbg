#include "common.h"

#include <sstream>
#include <vector>
#include <list>
#include <functional>
#include <iomanip>
#include <mutex>
#include <condition_variable>

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

    CORDB_ADDRESS startAddr = 0;
    CORDB_ADDRESS endAddr = 0;
    pFrame->GetStackRange(&startAddr, &endAddr);

    ss << "addr=\"0x" << std::setw(sizeof(void*)) << std::setfill('0') << std::hex << startAddr << "\"";

    output = ss.str();

    return has_source ? S_OK : S_FALSE;
}

enum FrameType
{
    FrameNative,
    FrameRuntimeUnwindable,
    FrameILStubOrLCG,
    FrameUnknown,
    FrameManaged
};

typedef std::function<HRESULT(FrameType,ICorDebugFrame*,ICorDebugILFrame*,ICorDebugFunction*)> WalkFramesCallback;

HRESULT WalkFrames(ICorDebugThread *pThread, WalkFramesCallback cb)
{
    HRESULT Status;

    ToRelease<ICorDebugThread3> pThread3;
    ToRelease<ICorDebugStackWalk> pStackWalk;

    IfFailRet(pThread->QueryInterface(IID_ICorDebugThread3, (LPVOID *) &pThread3));
    IfFailRet(pThread3->CreateStackWalk(&pStackWalk));

    for (Status = S_OK; ; Status = pStackWalk->Next())
    {
        if (Status == CORDBG_S_AT_END_OF_STACK)
            break;

        IfFailRet(Status);

        ToRelease<ICorDebugFrame> pFrame;
        IfFailRet(pStackWalk->GetFrame(&pFrame));
        if (Status == S_FALSE)
        {
            IfFailRet(cb(FrameNative, pFrame, nullptr, nullptr));
            continue;
        }

        ToRelease<ICorDebugRuntimeUnwindableFrame> pRuntimeUnwindableFrame;
        Status = pFrame->QueryInterface(IID_ICorDebugRuntimeUnwindableFrame, (LPVOID *) &pRuntimeUnwindableFrame);
        if (SUCCEEDED(Status))
        {
            IfFailRet(cb(FrameRuntimeUnwindable, pFrame, nullptr, nullptr));
            continue;
        }

        ToRelease<ICorDebugILFrame> pILFrame;
        HRESULT hrILFrame = pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame);

        if (FAILED(hrILFrame))
        {
            IfFailRet(cb(FrameUnknown, pFrame, nullptr, nullptr));
            continue;
        }

        ToRelease<ICorDebugFunction> pFunction;
        Status = pFrame->GetFunction(&pFunction);
        if (FAILED(Status))
        {
            IfFailRet(cb(FrameILStubOrLCG, pFrame, pILFrame, nullptr));
            continue;
        }

        IfFailRet(cb(FrameManaged, pFrame, pILFrame, pFunction));
    }

    return S_OK;
}

HRESULT GetFrameAt(ICorDebugThread *pThread, int level, ICorDebugFrame **ppFrame)
{
    ToRelease<ICorDebugFrame> result;

    int currentFrame = -1;

    HRESULT Status = WalkFrames(pThread, [&](
        FrameType frameType,
        ICorDebugFrame *pFrame,
        ICorDebugILFrame *pILFrame,
        ICorDebugFunction *pFunction)
    {
        currentFrame++;

        if (currentFrame < level)
            return S_OK;
        else if (currentFrame > level)
            return E_FAIL;

        if (currentFrame == level && frameType == FrameManaged)
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
        ICorDebugILFrame *pILFrame,
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
            case FrameNative:
                ss << "frame={level=\"" << currentFrame << "\",func=\"[NativeStackFrame]\"}";
                break;
            case FrameRuntimeUnwindable:
                ss << "frame={level=\"" << currentFrame << "\",func=\"[RuntimeUnwindableFrame]\"}";
                break;
            case FrameILStubOrLCG:
                ss << "frame={level=\"" << currentFrame << "\",func=\"[IL Stub or LCG]\"}";
                break;
            case FrameUnknown:
                ss << "frame={level=\"" << currentFrame << "\",func=\"?\"}";
                break;
            case FrameManaged:
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
