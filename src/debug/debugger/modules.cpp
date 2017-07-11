#include <windows.h>

#include "corhdr.h"
#include "cor.h"
#include "cordebug.h"
#include "debugshim.h"

#include <sstream>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <vector>

#include "torelease.h"
#include "symbolreader.h"
#include "cputil.h"

struct ModuleInfo
{
    CORDB_ADDRESS address;
    std::shared_ptr<SymbolReader> symbols;
    //ICorDebugModule *module;
};

std::mutex g_modulesInfoMutex;
std::unordered_map<std::string, ModuleInfo> g_modulesInfo;

void SetCoreCLRPath(const std::string &coreclrPath)
{
    SymbolReader::SetCoreCLRPath(coreclrPath);
}

std::string GetModuleName(ICorDebugModule *pModule)
{
    char cname[mdNameLen];
    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;
    if (SUCCEEDED(pModule->GetName(mdNameLen, &name_len, name)))
    {
        return to_utf8(name, name_len);
    }
    return std::string();
}

HRESULT GetLocationInModule(ICorDebugModule *pModule,
                            std::string filename,
                            ULONG linenum,
                            ULONG32 &ilOffset,
                            mdMethodDef &methodToken,
                            std::string &fullname)
{
    HRESULT Status;

    WCHAR nameBuffer[MAX_LONGPATH];

    Status = MultiByteToWideChar(CP_UTF8, 0, filename.c_str(), filename.size() + 1, nameBuffer, MAX_LONGPATH);

    std::string modName = GetModuleName(pModule);

    std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
    auto info_pair = g_modulesInfo.find(modName);
    if (info_pair == g_modulesInfo.end())
    {
        return E_FAIL;
    }

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));
    IfFailRet(info_pair->second.symbols->ResolveSequencePoint(nameBuffer, linenum, modAddress, &methodToken, &ilOffset));

    WCHAR wFilename[MAX_LONGPATH];
    ULONG resolvedLinenum;
    IfFailRet(info_pair->second.symbols->GetLineByILOffset(methodToken, ilOffset, &resolvedLinenum, wFilename, _countof(wFilename)));

    fullname = to_utf8(wFilename);

    return S_OK;
}

HRESULT GetFrameLocation(ICorDebugFrame *pFrame,
                         ULONG32 &ilOffset,
                         mdMethodDef &methodToken,
                         std::string &fullname,
                         ULONG &linenum)
{
    HRESULT Status;

    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&ilOffset, &mappingResult));

    std::string modName = GetModuleName(pModule);
    WCHAR name[MAX_LONGPATH];

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        auto info_pair = g_modulesInfo.find(modName);
        if (info_pair == g_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.symbols->GetLineByILOffset(methodToken, ilOffset, &linenum, name, _countof(name)));
    }

    fullname = to_utf8(name);

    return S_OK;
}

HRESULT GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));

    mdMethodDef methodToken;
    IfFailRet(pFrame->GetFunctionToken(&methodToken));

    ToRelease<ICorDebugFunction> pFunc;
    IfFailRet(pFrame->GetFunction(&pFunc));

    ToRelease<ICorDebugModule> pModule;
    IfFailRet(pFunc->GetModule(&pModule));

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID*) &pILFrame));

    ULONG32 nOffset;
    CorDebugMappingResult mappingResult;
    IfFailRet(pILFrame->GetIP(&nOffset, &mappingResult));

    ULONG32 ilStartOffset;
    ULONG32 ilEndOffset;

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        auto info_pair = g_modulesInfo.find(GetModuleName(pModule));
        if (info_pair == g_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.symbols->GetStepRangesFromIP(nOffset, methodToken, &ilStartOffset, &ilEndOffset));
    }

    if (ilStartOffset == ilEndOffset)
    {
        ToRelease<ICorDebugCode> pCode;
        IfFailRet(pFunc->GetILCode(&pCode));
        IfFailRet(pCode->GetSize(&ilEndOffset));
    }

    range->startOffset = ilStartOffset;
    range->endOffset = ilEndOffset;

    return S_OK;
}

HRESULT TryLoadModuleSymbols(ICorDebugModule *pModule)
{
    HRESULT Status;
    std::string name = GetModuleName(pModule);

    if (name.empty())
        return E_FAIL;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));

    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    auto symbolReader = std::make_shared<SymbolReader>();
    IfFailRet(symbolReader->LoadSymbols(pMDImport, pModule));

    CORDB_ADDRESS modAddress;
    pModule->GetBaseAddress(&modAddress);

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        g_modulesInfo.insert({name, {modAddress, symbolReader}});
    }
    return S_OK;
}

HRESULT GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue)
{
    HRESULT Status;

    WCHAR wParamName[mdNameLen] = W("\0");

    {
        std::lock_guard<std::mutex> lock(g_modulesInfoMutex);
        auto info_pair = g_modulesInfo.find(GetModuleName(pModule));
        if (info_pair == g_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.symbols->GetNamedLocalVariable(pILFrame, methodToken, localIndex, wParamName, _countof(wParamName), ppValue));
    }

    paramName = to_utf8(wParamName);

    return S_OK;
}
