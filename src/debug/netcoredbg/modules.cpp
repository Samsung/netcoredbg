// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "modules.h"

#include <sstream>
#include <vector>
#include <iomanip>
#include <algorithm>

#include "symbolreader.h"
#include "platform.h"
#include "cputil.h"
#include "typeprinter.h"


bool Modules::IsTargetFunction(const std::vector<std::string> &fullName, const std::vector<std::string> &targetName)
{
    // Function should be matched by substring, i.e. received target function name should fully or partly equal with the
    // real function name. For example:
    //
    // "MethodA" matches
    // Program.ClassA.MethodA
    // Program.ClassB.MethodA
    // Program.ClassA.InnerClass.MethodA
    //
    // "ClassA.MethodB" matches
    // Program.ClassA.MethodB
    // Program.ClassB.ClassA.MethodB

    auto fullIt = fullName.rbegin();
    for (auto it = targetName.rbegin(); it != targetName.rend(); it++)
    {
        if (fullIt == fullName.rend() || *it != *fullIt)
            return false;

        fullIt++;
    }

    return true;
}

HRESULT Modules::ResolveMethodInModule(ICorDebugModule *pModule, const std::string &funcName,
                                       ResolveFunctionBreakpointCallback cb)
{
    HRESULT Status;
    std::vector<std::string> splittedName = split_on_tokens(funcName, '.');
    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;

    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID *)&pMDImport));

    ULONG typesCnt = 0;
    HCORENUM fTypeEnum = NULL;
    mdTypeDef mdType = mdTypeDefNil;

    while (SUCCEEDED(pMDImport->EnumTypeDefs(&fTypeEnum, &mdType, 1, &typesCnt)) && typesCnt != 0)
    {
        std::string typeName;
        std::list<std::string> args;

        IfFailRet(TypePrinter::NameForToken(mdType, pMDImport, typeName, false, args));

        HCORENUM fFuncEnum = NULL;
        mdMethodDef mdMethod = mdMethodDefNil;
        ULONG methodsCnt = 0;

        while (SUCCEEDED(pMDImport->EnumMethods(&fFuncEnum, mdType, &mdMethod, 1, &methodsCnt)) && methodsCnt != 0)
        {
            mdTypeDef memTypeDef;
            ULONG nameLen;
            WCHAR szFuncName[mdNameLen] = {0};

            Status = pMDImport->GetMethodProps(mdMethod, &memTypeDef, szFuncName, _countof(szFuncName), &nameLen,
                                               nullptr, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(Status))
                continue;

            // Get generic types
            ToRelease<IMetaDataImport2> pMDImport2;

            IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport2, (LPVOID *)&pMDImport2));

            HCORENUM fGenEnum = NULL;
            mdGenericParam gp;
            ULONG fetched;
            std::string genParams("");

            while (SUCCEEDED(pMDImport2->EnumGenericParams(&fGenEnum, mdMethod, &gp, 1, &fetched)) && fetched == 1)
            {
                mdMethodDef memMethodDef;
                WCHAR szGenName[mdNameLen] = {0};
                ULONG genNameLen;

                Status = pMDImport2->GetGenericParamProps(gp, nullptr, nullptr, &memMethodDef, nullptr, szGenName, _countof(szGenName), &genNameLen);
                if (FAILED(Status))
                    continue;

                // Add comma for each element. The last one will be stripped later.
                genParams += to_utf8(szGenName) + ",";
            }

            pMDImport2->CloseEnum(fGenEnum);

            std::string fullName = to_utf8(szFuncName);
            if (genParams != "")
            {
                // Last symbol is comma and it is useless, so remove
                genParams.pop_back();
                fullName += "<" + genParams + ">";
            }

            std::vector<std::string> splittedFullName = split_on_tokens(typeName + "." + fullName, '.');

            // If we've found the target function
            if (IsTargetFunction(splittedFullName, splittedName))
            {
                if (FAILED(cb(pModule, mdMethod)))
                {
                    pMDImport->CloseEnum(fFuncEnum);
                    pMDImport->CloseEnum(fTypeEnum);

                    return E_FAIL;
                }
            }
        }

        pMDImport->CloseEnum(fFuncEnum);
    }
    pMDImport->CloseEnum(fTypeEnum);

    return S_OK;
}

void Modules::CleanupAllModules()
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    m_modulesInfo.clear();
}

std::string Modules::GetModuleFileName(ICorDebugModule *pModule)
{
    WCHAR name[mdNameLen];
    ULONG32 name_len = 0;

    if (FAILED(pModule->GetName(_countof(name), &name_len, name)))
        return std::string();

    std::string moduleName = to_utf8(name/*, name_len*/);

    // On Tizen platform module path may look like /proc/self/fd/8/bin/Xamarin.Forms.Platform.dll
    // This path is invalid in debugger process, we shoud change `self` to `<debugee process id>`
    static const std::string selfPrefix("/proc/self/");

    if (moduleName.compare(0, selfPrefix.size(), selfPrefix) != 0)
        return moduleName;

    ToRelease<ICorDebugProcess> pProcess;
    if (FAILED(pModule->GetProcess(&pProcess)))
        return std::string();

    DWORD pid = 0;

    if (FAILED(pProcess->GetID(&pid)))
        return std::string();

    std::ostringstream ss;
    ss << "/proc/" << pid << "/" << moduleName.substr(selfPrefix.size());
    return ss.str();
}

HRESULT Modules::GetLocationInAny(
    std::string filename,
    ULONG linenum,
    ULONG32 &ilOffset,
    mdMethodDef &methodToken,
    ICorDebugModule **ppModule)
{
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        CORDB_ADDRESS modAddress;
        IfFailRet(mdInfo.module->GetBaseAddress(&modAddress));
        if (FAILED(mdInfo.symbols->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset)))
            continue;

        SequencePoint resolvedSequencePoint;
        if (FAILED(GetSequencePointByILOffset(mdInfo.symbols.get(), methodToken, ilOffset, &resolvedSequencePoint)))
            continue;

        mdInfo.module->AddRef();
        *ppModule = mdInfo.module.GetPtr();
        return S_OK;
    }
    return E_FAIL;
}

HRESULT Modules::GetLocationInModule(
    ICorDebugModule *pModule,
    std::string filename,
    ULONG linenum,
    ULONG32 &ilOffset,
    mdMethodDef &methodToken)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(info_pair->second.symbols->ResolveSequencePoint(filename.c_str(), linenum, modAddress, &methodToken, &ilOffset));

    SequencePoint resolvedSequencePoint;
    IfFailRet(GetSequencePointByILOffset(info_pair->second.symbols.get(), methodToken, ilOffset, &resolvedSequencePoint));

    return S_OK;
}

HRESULT Modules::ResolveFunctionInAny(const std::string &module,
                                      const std::string &funcname,
                                      ResolveFunctionBreakpointCallback cb)
{
    bool isFull = IsFullPath(module);
    HRESULT Status;

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        ICorDebugModule *pModule = mdInfo.module.GetPtr();

        if (module != "") {
            ULONG32 nameLen;
            WCHAR szModuleName[mdNameLen] = {0};
            std::string modName;

            IfFailRet(pModule->GetName(_countof(szModuleName), &nameLen, szModuleName));

            if (isFull)
                modName = to_utf8(szModuleName);
            else
                modName = GetBasename(to_utf8(szModuleName));

            if (modName != module)
                continue;
        }

        if (SUCCEEDED(ResolveMethodInModule(mdInfo.module, funcname, cb)))
        {
            mdInfo.module->AddRef();
        }
    }

    return E_FAIL;
}


HRESULT Modules::ResolveFunctionInModule(ICorDebugModule *pModule,
                                         const std::string &module,
                                         std::string &funcname,
                                         ResolveFunctionBreakpointCallback cb)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;

    if (module != "")
    {
        ULONG32 len;
        WCHAR szModuleName[mdNameLen] = {0};
        std::string modName;

        IfFailRet(pModule->GetName(_countof(szModuleName), &len, szModuleName));

        if (IsFullPath(module))
            modName = to_utf8(szModuleName);
        else
            modName = GetBasename(to_utf8(szModuleName));

        if (modName != module)
            return E_FAIL;
    }

    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
        return E_FAIL;

    IfFailRet(ResolveMethodInModule(pModule, funcname, cb));

    return S_OK;
}

HRESULT Modules::GetFrameILAndSequencePoint(
    ICorDebugFrame *pFrame,
    ULONG32 &ilOffset,
    Modules::SequencePoint &sequencePoint)
{
    HRESULT Status;

    mdMethodDef methodToken;
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

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
    auto info_pair = m_modulesInfo.find(modAddress);
    if (info_pair == m_modulesInfo.end())
    {
        return E_FAIL;
    }

    IfFailRet(GetSequencePointByILOffset(info_pair->second.symbols.get(), methodToken, ilOffset, &sequencePoint));

    return S_OK;
}

HRESULT Modules::GetStepRangeFromCurrentIP(ICorDebugThread *pThread, COR_DEBUG_STEP_RANGE *range)
{
    HRESULT Status;
    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

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

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    ULONG32 ilStartOffset;
    ULONG32 ilEndOffset;

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
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

HRESULT Modules::GetModuleId(ICorDebugModule *pModule, std::string &id)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    GUID mvid;

    IfFailRet(pMDImport->GetScopeProps(nullptr, 0, nullptr, &mvid));

    std::ostringstream ss;
    ss << std::hex
    << std::setfill('0') << std::setw(8) << mvid.Data1 << "-"
    << std::setfill('0') << std::setw(4) << mvid.Data2 << "-"
    << std::setfill('0') << std::setw(4) << mvid.Data3 << "-"
    << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[0]) & 0xFF)
    << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[1]) & 0xFF)
    << "-";
    for (int i = 2; i < 8; i++)
        ss << std::setfill('0') << std::setw(2) << (static_cast<int>(mvid.Data4[i]) & 0xFF);

    id = ss.str();

    return S_OK;
}

HRESULT Modules::TryLoadModuleSymbols(
    ICorDebugModule *pModule,
    Module &module,
    bool needJMC
)
{
    HRESULT Status;

    ToRelease<IUnknown> pMDUnknown;
    ToRelease<IMetaDataImport> pMDImport;
    IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
    IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

    module.path = GetModuleFileName(pModule);
    module.name = GetFileName(module.path);

    std::unique_ptr<SymbolReader> symbolReader(new SymbolReader());

    symbolReader->LoadSymbols(pMDImport, pModule);
    module.symbolStatus = symbolReader->SymbolsLoaded() ? SymbolsLoaded : SymbolsNotFound;

    if (needJMC && module.symbolStatus == SymbolsLoaded)
    {
        // https://docs.microsoft.com/en-us/visualstudio/debugger/just-my-code
        // The .NET debugger considers optimized binaries and non-loaded .pdb files to be non-user code.
        // Three compiler attributes also affect what the .NET debugger considers to be user code:
        // * DebuggerNonUserCodeAttribute tells the debugger that the code it's applied to isn't user code.
        // * DebuggerHiddenAttribute hides the code from the debugger, even if Just My Code is turned off.
        // * DebuggerStepThroughAttribute tells the debugger to step through the code it's applied to, rather than step into the code.
        // The .NET debugger considers all other code to be user code.
        ToRelease<ICorDebugModule2> pModule2;
        if (SUCCEEDED(pModule->QueryInterface(IID_ICorDebugModule2, (LPVOID *)&pModule2)))
        {
            pModule2->SetJMCStatus(true, 0, nullptr);
            SetJMCFromAttributes(pModule, symbolReader.get());
        }
    }

    if (module.symbolStatus == SymbolsLoaded)
        IfFailRet(Modules::FillSourcesCodeLinesForModule(pMDImport, symbolReader.get()));

    IfFailRet(GetModuleId(pModule, module.id));

    CORDB_ADDRESS baseAddress;
    ULONG32 size;
    IfFailRet(pModule->GetBaseAddress(&baseAddress));
    IfFailRet(pModule->GetSize(&size));
    module.baseAddress = baseAddress;
    module.size = size;

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        pModule->AddRef();
        ModuleInfo mdInfo { std::move(symbolReader), pModule };
        m_modulesInfo.insert(std::make_pair(baseAddress, std::move(mdInfo)));
    }

    return S_OK;
}

HRESULT Modules::GetFrameNamedLocalVariable(
    ICorDebugModule *pModule,
    ICorDebugILFrame *pILFrame,
    mdMethodDef methodToken,
    ULONG localIndex,
    std::string &paramName,
    ICorDebugValue** ppValue,
    ULONG32 *pIlStart,
    ULONG32 *pIlEnd)
{
    HRESULT Status;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    WCHAR wParamName[mdNameLen] = W("\0");

    {
        std::lock_guard<std::mutex> lock(m_modulesInfoMutex);
        auto info_pair = m_modulesInfo.find(modAddress);
        if (info_pair == m_modulesInfo.end())
        {
            return E_FAIL;
        }

        IfFailRet(info_pair->second.symbols->GetNamedLocalVariableAndScope(pILFrame, methodToken, localIndex, wParamName, _countof(wParamName), ppValue, pIlStart, pIlEnd));
    }

    paramName = to_utf8(wParamName);

    return S_OK;
}

HRESULT Modules::GetModuleWithName(const std::string &name, ICorDebugModule **ppModule)
{
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;

        std::string path = GetModuleFileName(mdInfo.module);

        if (GetFileName(path) == name)
        {
            mdInfo.module->AddRef();
            *ppModule = mdInfo.module;
            return S_OK;
        }
    }
    return E_FAIL;
}

HRESULT Modules::GetSequencePointByILOffset(
    SymbolReader *symbols,
    mdMethodDef methodToken,
    ULONG32 &ilOffset,
    SequencePoint *sequencePoint)
{
    SymbolReader::SequencePoint symSequencePoint;

    if (FAILED(symbols->GetSequencePointByILOffset(methodToken, ilOffset, &symSequencePoint))) {
        return E_FAIL;
    }

    sequencePoint->document = to_utf8(symSequencePoint.document);
    sequencePoint->startLine = symSequencePoint.startLine;
    sequencePoint->startColumn = symSequencePoint.startColumn;
    sequencePoint->endLine = symSequencePoint.endLine;
    sequencePoint->endColumn = symSequencePoint.endColumn;
    sequencePoint->offset = symSequencePoint.offset;

    return S_OK;
}

HRESULT Modules::ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb)
{
    HRESULT Status;
    std::lock_guard<std::mutex> lock(m_modulesInfoMutex);

    for (auto &info_pair : m_modulesInfo)
    {
        ModuleInfo &mdInfo = info_pair.second;
        IfFailRet(cb(mdInfo.module));
    }
    return S_OK;
}

HRESULT Modules::FillSourcesCodeLinesForModule(IMetaDataImport *pMDImport, SymbolReader *symbolReader)
{
    HRESULT Status;
    ULONG numTypedefs = 0;
    HCORENUM fEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMDImport->EnumTypeDefs(&fEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        ULONG numMethods = 0;
        HCORENUM fEnum = NULL;
        mdMethodDef methodDef;
        while(SUCCEEDED(pMDImport->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
        {
            std::vector<SymbolReader::SequencePoint> points;
            if (FAILED(symbolReader->GetSequencePoints(methodDef, points)))
                continue;

            for (auto &point : points)
            {
                if (point.startLine == SymbolReader::HiddenLine)
                    continue;

                std::string fullPath = to_utf8(point.document);

#ifdef WIN32
                IfFailRet(SymbolReader::StringToUpper(fullPath));
#endif

                auto &codeLinesFullPath = m_sourcesCodeLines[fullPath];
                for (int i = point.startLine; i <= point.endLine; i++)
                    codeLinesFullPath[i] = point.startLine;

                m_sourcesFullPaths[GetFileName(fullPath)].push_back(std::move(fullPath));
            }
        }
        pMDImport->CloseEnum(fEnum);
    }
    pMDImport->CloseEnum(fEnum);

    return S_OK;
}

HRESULT Modules::ResolveRelativeSourceFileName(std::string &filename)
{
    auto searchByFileName = m_sourcesFullPaths.find(GetFileName(filename));
    if (searchByFileName == m_sourcesFullPaths.end())
        return E_FAIL;

    std::list<std::string> &possiblePaths = searchByFileName->second;
    std::string result = filename;

    // Care about all "./" and "../" first.
    std::list<std::string> pathDirs;
    std::size_t i;
    while ((i = result.find_first_of("/\\")) != std::string::npos)
    {
        std::string pathElement = result.substr(0, i);
        if (pathElement == "..")
        {
            if (!pathDirs.empty())
                pathDirs.pop_front();
        }
        else if (pathElement != ".")
            pathDirs.push_front(pathElement);

        result = result.substr(i + 1);
    }
    for (const auto &dir : pathDirs)
    {
        result = dir + '/' + result;
    }

    // The problem is - we could have several assemblies that could have same source file name with different path's root.
    // We don't really have a lot of options here, so, we assume, that all possible sources paths have same root and just find the shortest.
    if (result == GetFileName(result))
    {
        result = possiblePaths.front();
        for (const auto& path : possiblePaths)
        {
            if (result.length() > path.length())
                result = path;
        }

        filename = result;
        return S_OK;
    }

    std::list<std::string> possibleResults;
    for (auto &path : possiblePaths)
    {
        if (result.size() > path.size())
            continue;

        // Note, since assemblies could be built in different OSes, we could have different delimiters in source files paths.
        auto BinaryPredicate = [](const char& a, const char& b)
        {
            if ((a == '/' || a == '\\') && (b == '/' || b == '\\')) return true;
            return a == b;
        };

        // since C++17
        //if (std::equal(result.begin(), result.end(), path.end() - result.size(), BinaryPredicate))
        //    possibleResults.push_back(path);
        std::string::iterator first1 = result.begin();
        std::string::iterator last1 = result.end();
        std::string::iterator first2 = path.end() - result.size();
        auto equal = [&]()
        {
            for (; first1 != last1; ++first1, ++first2)
            {
                if (!BinaryPredicate(*first1, *first2))
                    return false;
            }
            return true;
        };
        if (equal())
            possibleResults.push_back(path);
    }
    // The problem is - we could have several assemblies that could have sources with same relative paths with different path's root.
    // We don't really have a lot of options here, so, we assume, that all possible sources paths have same root and just find the shortest.
    if (!possibleResults.empty())
    {
        filename = possibleResults.front();
        for (const auto& path : possibleResults)
        {
            if (filename.length() > path.length())
                filename = path;
        }
        return S_OK;
    }

    return E_FAIL;
}

HRESULT Modules::ResolveBreakpointFileAndLine(std::string &filename, int &linenum)
{
    HRESULT Status;
#ifdef WIN32
    IfFailRet(SymbolReader::StringToUpper(filename));
#endif

    auto searchByFullPath = m_sourcesCodeLines.find(filename);
    if (searchByFullPath == m_sourcesCodeLines.end())
    {
        // Check for absolute path.
#ifdef WIN32
        if (filename[1] == ':' && (filename[2] == '/' || filename[2] == '\\'))
#else
        if (filename[0] == '/')
#endif
        {
            return E_FAIL;
        }

        IfFailRet(ResolveRelativeSourceFileName(filename));

        searchByFullPath = m_sourcesCodeLines.find(filename);
        if (searchByFullPath == m_sourcesCodeLines.end())
            return E_FAIL;
    }

    auto &codeLines = searchByFullPath->second;

    auto resolvedLine = codeLines.lower_bound(linenum);
    if (resolvedLine == codeLines.end())
        return E_FAIL;

    linenum = resolvedLine->second;

    return S_OK;
}
