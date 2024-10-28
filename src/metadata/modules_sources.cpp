// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <list>
#include <map>
#include <memory>
#include <algorithm>
#include <fstream>

#include "metadata/modules_sources.h"
#include "metadata/modules.h"
#include "metadata/jmc.h"
#include "managed/interop.h"
#include "utils/utf.h"

namespace netcoredbg
{

namespace
{

    struct file_methods_data_t
    {
        BSTR document;
        int32_t methodNum;
        method_data_t *methodsData;

        file_methods_data_t() = delete;
        file_methods_data_t(const file_methods_data_t&) = delete;
        file_methods_data_t& operator=(const file_methods_data_t&) = delete;
        file_methods_data_t(file_methods_data_t&&) = delete;
        file_methods_data_t& operator=(file_methods_data_t&&) = delete;
    };

    struct module_methods_data_t
    {
        int32_t fileNum;
        file_methods_data_t *moduleMethodsData;

        module_methods_data_t() = delete;
        module_methods_data_t(const module_methods_data_t&) = delete;
        module_methods_data_t& operator=(const module_methods_data_t&) = delete;
        module_methods_data_t(module_methods_data_t&&) = delete;
        module_methods_data_t& operator=(module_methods_data_t&&) = delete;
    };

    struct module_methods_data_t_deleter
    {
        void operator()(module_methods_data_t *p) const
        {
            if (p->moduleMethodsData)
            {
                for (int32_t i = 0; i < p->fileNum; i++)
                {
                    if (p->moduleMethodsData[i].document)
                        Interop::SysFreeString(p->moduleMethodsData[i].document);
                    if (p->moduleMethodsData[i].methodsData)
                        Interop::CoTaskMemFree(p->moduleMethodsData[i].methodsData);
                }
                Interop::CoTaskMemFree(p->moduleMethodsData);
            }
            Interop::CoTaskMemFree(p);
        }
    };

    // Note, we use std::map since we need container that will not invalidate iterators on add new elements.
    void AddMethodData(/*in,out*/ std::map<size_t, std::set<method_data_t>> &methodData,
                       /*in,out*/ std::unordered_map<method_data_t, std::vector<mdMethodDef>, method_data_t_hash> &multiMethodBpData,
                       const method_data_t &entry,
                       const size_t nestedLevel)
    {
        // if we here, we need at least one nested level for sure
        if (methodData.empty())
        {
            methodData.emplace(std::make_pair(0, std::set<method_data_t>{entry}));
            return;
        }
        assert(nestedLevel <= methodData.size()); // could be increased only at 1 per recursive call
        if (nestedLevel == methodData.size())
        {
            methodData.emplace(std::make_pair(nestedLevel, std::set<method_data_t>{entry}));
            return;
        }

        // same data that was already added, but with different method token (constructors case)
        auto find = methodData[nestedLevel].find(entry);
        if (find != methodData[nestedLevel].end())
        {
            method_data_t key(find->methodDef, entry.startLine, entry.endLine, entry.startColumn, entry.endColumn);
            auto find_multi = multiMethodBpData.find(key);
            if (find_multi == multiMethodBpData.end())
                multiMethodBpData.emplace(std::make_pair(key, std::vector<mdMethodDef>{entry.methodDef}));
            else
                find_multi->second.emplace_back(entry.methodDef);

            return;
        }

        auto it = methodData[nestedLevel].lower_bound(entry);
        if (it != methodData[nestedLevel].end() && entry.NestedInto(*it))
        {
            AddMethodData(methodData, multiMethodBpData, entry, nestedLevel + 1);
            return;
        }

        // case with only one element on nested level, NestedInto() was already called and entry checked
        if (it == methodData[nestedLevel].begin())
        {
            methodData[nestedLevel].emplace(entry);
            return;
        }

        // move all previously added nested for new entry elements to level above
        do
        {
            it = std::prev(it);

            if ((*it).NestedInto(entry))
            {
                method_data_t tmp = *it;
                it = methodData[nestedLevel].erase(it);
                AddMethodData(methodData, multiMethodBpData, tmp, nestedLevel + 1);
            }
            else
                break;
        }
        while(it != methodData[nestedLevel].begin());

        methodData[nestedLevel].emplace(entry);
    }


    bool GetMethodTokensByLineNumber(const std::vector<std::vector<method_data_t>> &methodBpData,
                                     const std::unordered_map<method_data_t, std::vector<mdMethodDef>, method_data_t_hash> &multiMethodBpData,
                                     /*in,out*/ int32_t &lineNum,
                                     /*out*/ std::vector<mdMethodDef> &Tokens,
                                     /*out*/ mdMethodDef &closestNestedToken)
    {
        const method_data_t *result = nullptr;
        closestNestedToken = 0;

        for (auto it = methodBpData.cbegin(); it != methodBpData.cend(); ++it)
        {
            auto lower = std::lower_bound((*it).cbegin(), (*it).cend(), lineNum);
            if (lower == (*it).cend())
                break; // point behind last method for this nested level

            // case with first line of method, for example:
            // void Method(){ 
            //            void Method(){ void Method(){...  <- breakpoint at this line
            if (lineNum == (*lower).startLine)
            {
                // At this point we can't check this case, let managed part decide (since it see Columns):
                // void Method() {
                // ... code ...; void Method() {     <- breakpoint at this line
                //  };
                if (result)
                    closestNestedToken = (*lower).methodDef;
                else
                    result = &(*lower);

                break;
            }
            else if (lineNum > (*lower).startLine && (*lower).endLine >= lineNum)
            {
                result = &(*lower);
                continue; // need check nested level (if available)
            }
            // out of first level methods lines - forced move line to first method below, for example:
            //  <-- breakpoint at line without code (out of any methods)
            // void Method() {...}
            else if (it == methodBpData.cbegin() && lineNum < (*lower).startLine)
            {
                lineNum = (*lower).startLine;
                result = &(*lower);
                break;
            }
            // result was found on previous cycle, check for closest nested method
            // need it in case of breakpoint setuped at lines without code and before nested method, for example:
            // {
            //  <-- breakpoint at line without code (inside method)
            //     void Method() {...}
            // }
            else if (result && lineNum <= (*lower).startLine && (*lower).endLine <= result->endLine)
            {
                closestNestedToken = (*lower).methodDef;
                break;
            }
            else
                break;
        }

        if (result)
        {
            auto find = multiMethodBpData.find(*result);
            if (find != multiMethodBpData.end()) // only constructors segments could be part of multiple methods
            {
                Tokens.resize((*find).second.size());
                std::copy(find->second.begin(), find->second.end(), Tokens.begin());
            }
            Tokens.emplace_back(result->methodDef);
        }
        
        return !!result;
    }

} // unnamed namespace

static HRESULT GetPdbMethodsRanges(IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle, std::unordered_set<mdMethodDef> *methodTokens,
                                   std::unique_ptr<module_methods_data_t, module_methods_data_t_deleter> &inputData)
{
    HRESULT Status;
    // Note, we need 2 arrays of tokens - for normal methods and constructors (.ctor/.cctor, that could have segmented code).
    std::vector<int32_t> constrTokens;
    std::vector<int32_t> normalTokens;

    ULONG numTypedefs = 0;
    HCORENUM hEnum = NULL;
    mdTypeDef typeDef;
    while(SUCCEEDED(pMDImport->EnumTypeDefs(&hEnum, &typeDef, 1, &numTypedefs)) && numTypedefs != 0)
    {
        ULONG numMethods = 0;
        HCORENUM fEnum = NULL;
        mdMethodDef methodDef;
        while(SUCCEEDED(pMDImport->EnumMethods(&fEnum, typeDef, &methodDef, 1, &numMethods)) && numMethods != 0)
        {
            if (methodTokens && methodTokens->find(methodDef) == methodTokens->end())
                continue;

            WCHAR funcName[mdNameLen];
            ULONG funcNameLen;
            if (FAILED(pMDImport->GetMethodProps(methodDef, nullptr, funcName, _countof(funcName), &funcNameLen,
                                                 nullptr, nullptr, nullptr, nullptr, nullptr)))
            {
                continue;
            }

            if (str_equal(funcName, W(".ctor")) || str_equal(funcName, W(".cctor")))
                constrTokens.emplace_back(methodDef);
            else
                normalTokens.emplace_back(methodDef);
        }
        pMDImport->CloseEnum(fEnum);
    }
    pMDImport->CloseEnum(hEnum);

    if (sizeof(std::size_t) > sizeof(std::uint32_t) &&
        (constrTokens.size() > std::numeric_limits<uint32_t>::max() || normalTokens.size() > std::numeric_limits<uint32_t>::max()))
    {
        LOGE("Too big token arrays.");
        return E_FAIL;
    }

    PVOID data = nullptr;
    IfFailRet(Interop::GetModuleMethodsRanges(pSymbolReaderHandle, (uint32_t)constrTokens.size(), constrTokens.data(), (uint32_t)normalTokens.size(), normalTokens.data(), &data));
    if (data == nullptr)
        return S_OK;

    inputData.reset((module_methods_data_t*)data);
    return S_OK;
}

static std::string GetFileName(const std::string &path)
{
    std::size_t i = path.find_last_of("/\\");
    return i == std::string::npos ? path : path.substr(i + 1);
}

// Caller must care about m_sourcesInfoMutex.
HRESULT ModulesSources::GetFullPathIndex(BSTR document, unsigned &fullPathIndex)
{
    std::string fullPath = to_utf8(document);
#ifdef WIN32
    HRESULT Status;
    std::string initialFullPath = fullPath;
    IfFailRet(Interop::StringToUpper(fullPath));
#endif
    auto findPathIndex = m_sourcePathToIndex.find(fullPath);
    if (findPathIndex == m_sourcePathToIndex.end())
    {
        fullPathIndex = (unsigned)m_sourceIndexToPath.size();
        m_sourcePathToIndex.emplace(std::make_pair(fullPath, fullPathIndex));
        m_sourceIndexToPath.emplace_back(fullPath);
#ifdef WIN32
        m_sourceIndexToInitialFullPath.emplace_back(initialFullPath);
#endif
        m_sourceNameToFullPathsIndexes[GetFileName(fullPath)].emplace(fullPathIndex);
        m_sourcesMethodsData.emplace_back(std::vector<FileMethodsData>{});
    }
    else
        fullPathIndex = findPathIndex->second;

    return S_OK;
}

HRESULT ModulesSources::FillSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    HRESULT Status;
    std::unique_ptr<module_methods_data_t, module_methods_data_t_deleter> inputData;
    IfFailRet(GetPdbMethodsRanges(pMDImport, pSymbolReaderHandle, nullptr, inputData));
    if (inputData == nullptr)
        return S_OK;

    // Usually, modules provide files with unique full paths for sources.
    m_sourceIndexToPath.reserve(m_sourceIndexToPath.size() + inputData->fileNum);
    m_sourcesMethodsData.reserve(m_sourcesMethodsData.size() + inputData->fileNum);
#ifdef WIN32
    m_sourceIndexToInitialFullPath.reserve(m_sourceIndexToInitialFullPath.size() + inputData->fileNum);
#endif

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    for (int i = 0; i < inputData->fileNum; i++)
    {
        unsigned fullPathIndex;
        IfFailRet(GetFullPathIndex(inputData->moduleMethodsData[i].document, fullPathIndex));

        m_sourcesMethodsData[fullPathIndex].emplace_back(FileMethodsData{});
        auto &fileMethodsData = m_sourcesMethodsData[fullPathIndex].back();
        fileMethodsData.modAddress = modAddress;

        // Note, don't reorder input data, since it have almost ideal order for us.
        // For example, for Private.CoreLib (about 22000 methods) only 8 relocations were made.
        // In case default methods ordering will be dramatically changed, we could use data reordering,
        // for example based on this solution:
        //    struct compare {
        //        bool operator()(const method_data_t &lhs, const method_data_t &rhs) const
        //        { return lhs.endLine > rhs.endLine || (lhs.endLine == rhs.endLine && lhs.endColumn > rhs.endColumn); }
        //    };
        //    std::multiset<method_data_t, compare> orderedInputData;
        std::map<size_t, std::set<method_data_t>> inputMethodsData;
        for (int j = 0; j < inputData->moduleMethodsData[i].methodNum; j++)
        {
            AddMethodData(inputMethodsData, fileMethodsData.multiMethodsData, inputData->moduleMethodsData[i].methodsData[j], 0);
        }

        fileMethodsData.methodsData.resize(inputMethodsData.size());
        for (size_t i =  0; i < inputMethodsData.size(); i++)
        {
            fileMethodsData.methodsData[i].resize(inputMethodsData[i].size());
            std::copy(inputMethodsData[i].begin(), inputMethodsData[i].end(), fileMethodsData.methodsData[i].begin());
        }
        for (auto &data : fileMethodsData.multiMethodsData)
        {
            data.second.shrink_to_fit();
        }
    }

    m_sourcesMethodsData.shrink_to_fit();
    m_sourceIndexToPath.shrink_to_fit();
#ifdef WIN32
    m_sourceIndexToInitialFullPath.shrink_to_fit();
#endif

    return S_OK;
}

HRESULT ModulesSources::LineUpdatesForMethodData(ICorDebugModule *pModule, unsigned fullPathIndex, method_data_t &methodData,
                                                 const std::vector<block_update_t> &blockUpdate, ModuleInfo &mdInfo)
{
    int32_t startLineOffset = 0;
    int32_t endLineOffset = 0;
    std::unordered_map<std::size_t, int32_t> methodBlockOffsets;

    for (const auto &block : blockUpdate)
    {
        if (block.oldLine < 0 || block.endLineOffset < 0 || methodData.endLine < 0)
            return E_INVALIDARG;

        const int32_t lineOffset = block.newLine - block.oldLine;

        // Note, endLineOffset could be std::numeric_limits<int32_t>::max() (max line number in C# source), so, we forced to cast it first.
        // Also, this is why we test that method within range and after that negate result.
        if ((uint32_t)methodData.startLine <= (uint32_t)block.oldLine + (uint32_t)block.endLineOffset && methodData.startLine >= block.oldLine)
        {
            startLineOffset = lineOffset;
        }

        if ((uint32_t)methodData.endLine <= (uint32_t)block.oldLine + (uint32_t)block.endLineOffset && methodData.endLine >= block.oldLine)
        {
            endLineOffset = lineOffset;
        }

        if ((uint32_t)methodData.startLine > (uint32_t)block.oldLine + (uint32_t)block.endLineOffset || methodData.endLine < block.oldLine)
            continue;

        // update methodBlockUpdates

        auto findMethod = mdInfo.m_methodBlockUpdates.find(methodData.methodDef);
        if (findMethod == mdInfo.m_methodBlockUpdates.end())
        {
            HRESULT Status;
            ToRelease<ICorDebugFunction> iCorFunction;
            IfFailRet(pModule->GetFunctionFromToken(methodData.methodDef, &iCorFunction));
            ToRelease<ICorDebugFunction2> iCorFunction2;
            IfFailRet(iCorFunction->QueryInterface(IID_ICorDebugFunction2, (LPVOID*) &iCorFunction2));
            ULONG32 methodVersion;
            IfFailRet(iCorFunction2->GetVersionNumber(&methodVersion));

            if (mdInfo.m_symbolReaderHandles.empty() || mdInfo.m_symbolReaderHandles.size() < methodVersion)
                return E_FAIL;

            Interop::SequencePoint *sequencePoints = nullptr;
            int32_t count = 0;
            if (FAILED(Status = Interop::GetSequencePoints(mdInfo.m_symbolReaderHandles[methodVersion - 1], methodData.methodDef, &sequencePoints, count)))
            {
                if (sequencePoints)
                {
                    for (int i = 0; i < count; i++)
                    {
                        Interop::SysFreeString(sequencePoints[i].document);
                    }
                    Interop::CoTaskMemFree(sequencePoints);
                }

                return Status;
            }

            for (int i = 0; i < count; i++)
            {
                unsigned index;
                IfFailRet(GetFullPathIndex(sequencePoints[i].document, index));
                Interop::SysFreeString(sequencePoints[i].document);

                mdInfo.m_methodBlockUpdates[methodData.methodDef].emplace_back(index, sequencePoints[i].startLine, sequencePoints[i].startLine, sequencePoints[i].endLine - sequencePoints[i].startLine);

            }

            if (sequencePoints)
                Interop::CoTaskMemFree(sequencePoints);
        }

        for (std::size_t i = 0; i < mdInfo.m_methodBlockUpdates[methodData.methodDef].size(); ++i)
        {
            auto &entry = mdInfo.m_methodBlockUpdates[methodData.methodDef][i];

            if (entry.fullPathIndex != fullPathIndex ||
                entry.newLine < block.oldLine ||
                (uint32_t)entry.newLine + (uint32_t)entry.endLineOffset > (uint32_t)block.oldLine + (uint32_t)block.endLineOffset)
                continue;

            // Line updates file can have only one entry for each line.
            methodBlockOffsets[i] = lineOffset;
        }
    }

    if (!methodBlockOffsets.empty())
    {
        auto findMethod = mdInfo.m_methodBlockUpdates.find(methodData.methodDef);
        assert(findMethod != mdInfo.m_methodBlockUpdates.end());

        for (const auto &entry : methodBlockOffsets)
        {
            // All we need for previous stored data is change newLine, since oldLine will be the same (PDB for this method version was not changed).
            findMethod->second[entry.first].newLine += entry.second;
        }
    }

    if (startLineOffset == 0 && endLineOffset == 0)
        return S_OK;

    methodData.startLine += startLineOffset;
    methodData.endLine += endLineOffset;
    return S_OK;
}

HRESULT ModulesSources::UpdateSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, std::unordered_set<mdMethodDef> methodTokens,
                                                        src_block_updates_t &srcBlockUpdates, ModuleInfo &mdInfo)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    HRESULT Status;
    std::unique_ptr<module_methods_data_t, module_methods_data_t_deleter> inputData;
    IfFailRet(GetPdbMethodsRanges(pMDImport, mdInfo.m_symbolReaderHandles.back(), &methodTokens, inputData));

    struct src_update_data_t
    {
        std::vector<block_update_t> blockUpdate;
        int32_t methodNum = 0;
        method_data_t *methodsData = nullptr;
    };
    std::unordered_map<unsigned, src_update_data_t> srcUpdateData;

    if (inputData)
    {
        for (int i = 0; i < inputData->fileNum; i++)
        {
            unsigned fullPathIndex;
            IfFailRet(GetFullPathIndex(inputData->moduleMethodsData[i].document, fullPathIndex));

            srcUpdateData[fullPathIndex].methodNum = inputData->moduleMethodsData[i].methodNum;
            srcUpdateData[fullPathIndex].methodsData = inputData->moduleMethodsData[i].methodsData;
        }
    }
    for (const auto &entry : srcBlockUpdates)
    {
        srcUpdateData[entry.first].blockUpdate = entry.second;
    }

    if (srcUpdateData.empty())
        return S_OK;

    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    for (const auto &updateData : srcUpdateData)
    {
        const unsigned fullPathIndex = updateData.first;

        std::map<size_t, std::set<method_data_t>> inputMethodsData;
        if (m_sourcesMethodsData[fullPathIndex].empty())
        { // New source file added.
            m_sourcesMethodsData[fullPathIndex].emplace_back(FileMethodsData{});
            auto &tmpFileMethodsData = m_sourcesMethodsData[fullPathIndex].back();
            tmpFileMethodsData.modAddress = modAddress;

            for (int j = 0; j < updateData.second.methodNum; j++)
            {
                AddMethodData(inputMethodsData, tmpFileMethodsData.multiMethodsData, updateData.second.methodsData[j], 0);
            }
        }
        else
        {
            std::unordered_set<mdMethodDef> inputMetodDefSet;
            for (int j = 0; j < updateData.second.methodNum; j++)
            {
                inputMetodDefSet.insert(updateData.second.methodsData[j].methodDef);
                // All sequence points related to this method were updated and provide proper lines from PDB directly.
                mdInfo.m_methodBlockUpdates.erase(updateData.second.methodsData[j].methodDef);
            }

            // Move multiMethodsData first (since this is constructors and all will be on level 0 for sure).
            // Use std::unordered_set here instead array for fast search.
            auto &tmpFileMethodsData = m_sourcesMethodsData[fullPathIndex].back();
            std::vector<method_data_t> tmpMultiMethodsData;
            for (const auto &entryData : tmpFileMethodsData.multiMethodsData)
            {
                auto findData = inputMetodDefSet.find(entryData.first.methodDef);
                if (findData == inputMetodDefSet.end())
                    tmpMultiMethodsData.emplace_back(entryData.first);

                for (const auto &entryMethodDef : entryData.second)
                {
                    findData = inputMetodDefSet.find(entryMethodDef);
                    if (findData == inputMetodDefSet.end())
                        tmpMultiMethodsData.emplace_back(entryMethodDef, entryData.first.startLine, entryData.first.endLine,
                                                         entryData.first.startColumn, entryData.first.endColumn);
                }
            }

            tmpFileMethodsData.multiMethodsData.clear();
            for (auto &methodData : tmpMultiMethodsData)
            {
                IfFailRet(LineUpdatesForMethodData(pModule, fullPathIndex, methodData, updateData.second.blockUpdate, mdInfo));
                AddMethodData(inputMethodsData, tmpFileMethodsData.multiMethodsData, methodData, 0);
            }

            // Move normal methods.
            for (auto &methodsData : tmpFileMethodsData.methodsData)
            {
                for (auto &methodData : methodsData)
                {
                    auto findData = inputMetodDefSet.find(methodData.methodDef);
                    if (findData == inputMetodDefSet.end())
                    {
                        IfFailRet(LineUpdatesForMethodData(pModule, fullPathIndex, methodData, updateData.second.blockUpdate, mdInfo));
                        AddMethodData(inputMethodsData, tmpFileMethodsData.multiMethodsData, methodData, 0);
                    }
                }
            }
            tmpFileMethodsData.methodsData.clear();

            // Move new and modified methods.
            for (int j = 0; j < updateData.second.methodNum; j++)
            {
                AddMethodData(inputMethodsData, tmpFileMethodsData.multiMethodsData, updateData.second.methodsData[j], 0);
            }
        }

        auto &fileMethodsData = m_sourcesMethodsData[fullPathIndex].back();
        fileMethodsData.methodsData.resize(inputMethodsData.size());
        for (size_t i =  0; i < inputMethodsData.size(); i++)
        {
            fileMethodsData.methodsData[i].resize(inputMethodsData[i].size());
            std::copy(inputMethodsData[i].begin(), inputMethodsData[i].end(), fileMethodsData.methodsData[i].begin());
        }
        for (auto &data : fileMethodsData.multiMethodsData)
        {
            data.second.shrink_to_fit();
        }
    }

    return S_OK;
}

HRESULT ModulesSources::ResolveRelativeSourceFileName(std::string &filename)
{
    // IMPORTANT! Caller should care about m_sourcesInfoMutex.
    auto findIndexesByFileName = m_sourceNameToFullPathsIndexes.find(GetFileName(filename));
    if (findIndexesByFileName == m_sourceNameToFullPathsIndexes.end())
        return E_FAIL;

    auto const &possiblePathsIndexes = findIndexesByFileName->second;
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
        auto it = std::min_element(possiblePathsIndexes.begin(), possiblePathsIndexes.end(),
                        [&](const unsigned a, const unsigned b){ return m_sourceIndexToPath[a].size() < m_sourceIndexToPath[b].size(); } );

        filename = it == possiblePathsIndexes.end() ? result : m_sourceIndexToPath[*it];
        return S_OK;
    }

    std::list<std::string> possibleResults;
    for (const auto pathIndex : possiblePathsIndexes)
    {
        if (result.size() > m_sourceIndexToPath[pathIndex].size())
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
        auto first1 = result.begin();
        auto last1 = result.end();
        auto first2 = m_sourceIndexToPath[pathIndex].end() - result.size();
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
            possibleResults.push_back(m_sourceIndexToPath[pathIndex]);
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

// Note, this is breakpoint only backward correction, that will care for "closest next executable code line" in PDB stored data.
// We can't map line from new to old PDB location, since impossible map new added line data to PDB data that don't have this line.
// Plus, methodBlockUpdates store sequence points only data.
static void LineUpdatesBackwardCorrection(unsigned fullPathIndex, mdMethodDef methodToken, method_block_updates_t &methodBlockUpdates, int32_t &startLine)
{
    auto findSourceUpdate = methodBlockUpdates.find(methodToken);
    if (findSourceUpdate != methodBlockUpdates.end())
    {
        for (const auto &entry : findSourceUpdate->second)
        {
            if (entry.fullPathIndex != fullPathIndex ||
                entry.newLine + entry.endLineOffset < startLine)
                continue;

            startLine = entry.oldLine; // <- closest executable code line for requested line in old PDB data
            break;
        }
    }
}

HRESULT ModulesSources::ResolveBreakpoint(/*in*/ Modules *pModules, /*in*/ CORDB_ADDRESS modAddress, /*in*/ std::string filename, /*out*/ unsigned &fullname_index,
                                          /*in*/ int sourceLine, /*out*/ std::vector<resolved_bp_t> &resolvedPoints)
{
    std::lock_guard<std::mutex> lockSourcesInfo(m_sourcesInfoMutex);

    HRESULT Status;
    auto findIndex = m_sourcePathToIndex.find(filename);
    if (findIndex == m_sourcePathToIndex.end())
    {
        // Check for absolute path.
#ifdef WIN32
        // Check, if start from drive letter, for example "D:\" or "D:/".
        if (filename.size() > 2 && filename[1] == ':' && (filename[2] == '/' || filename[2] == '\\'))
#else
        if (filename[0] == '/')
#endif
        {
            return E_FAIL;
        }

        IfFailRet(ResolveRelativeSourceFileName(filename));

        findIndex = m_sourcePathToIndex.find(filename);
        if (findIndex == m_sourcePathToIndex.end())
            return E_FAIL;
    }

    fullname_index = findIndex->second;

    struct resolved_input_bp_t
    {
        int32_t startLine;
        int32_t endLine;
        uint32_t ilOffset;
        uint32_t methodToken;
    };

    struct resolved_input_bp_t_deleter
    {
        void operator()(resolved_input_bp_t *p) const
        {
            Interop::CoTaskMemFree(p);
        }
    };

    for (const auto &sourceData : m_sourcesMethodsData[findIndex->second])
    {
        if (modAddress && modAddress != sourceData.modAddress)
            continue;

        std::vector<mdMethodDef> Tokens;
        int32_t correctedStartLine = sourceLine;
        mdMethodDef closestNestedToken = 0;
        if (!GetMethodTokensByLineNumber(sourceData.methodsData, sourceData.multiMethodsData, correctedStartLine, Tokens, closestNestedToken))
            continue;
        // correctedStartLine - in case line not belong any methods, if possible, will be "moved" to first line of method below sourceLine.

        if ((int32_t)Tokens.size() > std::numeric_limits<int32_t>::max())
        {
            LOGE("Too big token arrays.");
            return E_FAIL;
        }

        ModuleInfo *pmdInfo; // Note, pmdInfo must be covered by m_modulesInfoMutex.
        IfFailRet(pModules->GetModuleInfo(sourceData.modAddress, &pmdInfo)); // we must have it, since we loaded data from it
        if (pmdInfo->m_symbolReaderHandles.empty())
            continue;

        // In case one source line (field/property initialization) compiled into all constructors, after Hot Reload, constructors may have different
        // code version numbers, that mean debug info located in different symbol readers.
        std::vector<PVOID> symbolReaderHandles;
        symbolReaderHandles.reserve(Tokens.size());
        for (auto methodToken : Tokens)
        {
            // Note, new breakpoints could be setup for last code version only, since protocols (MI, VSCode, ...) provide source:line data only.
            ULONG32 currentVersion;
            ToRelease<ICorDebugFunction> pFunction;
            if (FAILED(pmdInfo->m_iCorModule->GetFunctionFromToken(methodToken, &pFunction)) ||
                FAILED(pFunction->GetCurrentVersionNumber(&currentVersion)))
            {
                symbolReaderHandles.emplace_back(pmdInfo->m_symbolReaderHandles[0]);
                continue;
            }

            assert(pmdInfo->m_symbolReaderHandles.size() >= currentVersion);
            symbolReaderHandles.emplace_back(pmdInfo->m_symbolReaderHandles[currentVersion - 1]);
        }

        // In case Hot Reload we may have line updates that we must take into account.
        LineUpdatesBackwardCorrection(findIndex->second, Tokens[0], pmdInfo->m_methodBlockUpdates, correctedStartLine);

        PVOID data = nullptr;
        int32_t Count = 0;
#ifndef _WIN32
        std::string fullName = m_sourceIndexToPath[findIndex->second];
#else
        std::string fullName = m_sourceIndexToInitialFullPath[findIndex->second];
#endif
        if (FAILED(Interop::ResolveBreakPoints(symbolReaderHandles.data(), (int32_t)Tokens.size(), Tokens.data(),
                                               correctedStartLine, closestNestedToken, Count, fullName, &data))
            || data == nullptr)
        {
            continue;
        }
        std::unique_ptr<resolved_input_bp_t, resolved_input_bp_t_deleter> inputData((resolved_input_bp_t*)data);

        for (int32_t i = 0; i < Count; i++)
        {
            pmdInfo->m_iCorModule->AddRef();

            // In case Hot Reload we may have line updates that we must take into account.
            LineUpdatesForwardCorrection(findIndex->second, inputData.get()[i].methodToken, pmdInfo->m_methodBlockUpdates, inputData.get()[i]);

            resolvedPoints.emplace_back(resolved_bp_t(inputData.get()[i].startLine, inputData.get()[i].endLine, inputData.get()[i].ilOffset,
                                                      inputData.get()[i].methodToken, pmdInfo->m_iCorModule.GetPtr()));
        }
    }

    return S_OK;
}

static HRESULT LoadLineUpdatesFile(ModulesSources *pModulesSources, const std::string &lineUpdates, src_block_updates_t &srcBlockUpdates)
{
    std::ifstream lineUpdatesFileStream(lineUpdates, std::ios::in | std::ios::binary);
    if (!lineUpdatesFileStream.is_open())
        return COR_E_FILENOTFOUND;

    HRESULT Status;
    uint32_t sourcesCount = 0;
    lineUpdatesFileStream.read((char*)&sourcesCount, 4);
    if (lineUpdatesFileStream.fail())
        return E_FAIL;

    struct line_update_t
    {
        int32_t newLine;
        int32_t oldLine;
    };
    std::unordered_map<unsigned /*source fullPathIndex*/, std::vector<line_update_t>> lineUpdatesData;

    // 0xfeefee is a magic number for "#line hidden" directive.
    // https://docs.microsoft.com/en-us/dotnet/csharp/language-reference/preprocessor-directives/preprocessor-line
    // https://docs.microsoft.com/en-us/archive/blogs/jmstall/line-hidden-and-0xfeefee-sequence-points
    if (sourcesCount >= 0xfeefee)
        return E_FAIL;

    for (uint32_t i = 0; i < sourcesCount; i++)
    {
        uint32_t stringSize = 0;
        lineUpdatesFileStream.read((char*)&stringSize, 4);
        if (lineUpdatesFileStream.fail())
            return E_FAIL;

        std::string fullPath;
        fullPath.resize(stringSize);
        lineUpdatesFileStream.read((char*)fullPath.data(), stringSize);
        if (lineUpdatesFileStream.fail())
            return E_FAIL;

        unsigned fullPathIndex;
        IfFailRet(pModulesSources->GetIndexBySourceFullPath(fullPath, fullPathIndex));

        uint32_t updatesCount = 0;
        lineUpdatesFileStream.read((char*)&updatesCount, 4);
        if (lineUpdatesFileStream.fail())
            return E_FAIL;

        if (updatesCount == 0)
            continue;

        std::vector<line_update_t> &lineUpdates = lineUpdatesData[fullPathIndex];
        lineUpdates.resize(updatesCount);

        lineUpdatesFileStream.read((char*)lineUpdates.data(), updatesCount * sizeof(line_update_t));
        if (lineUpdatesFileStream.fail())
            return E_FAIL;
    }

    line_update_t startBlock;
    const int32_t empty = -1;
    for (const auto &updateFileData : lineUpdatesData)
    {
        startBlock.newLine = empty;
        for (const auto &lineUpdates : updateFileData.second)
        {
            if (lineUpdates.newLine != lineUpdates.oldLine)
            {
                if (startBlock.newLine != empty)
                    srcBlockUpdates[updateFileData.first].emplace_back(startBlock.newLine + 1, startBlock.oldLine + 1, lineUpdates.oldLine - 1 - startBlock.oldLine);

                startBlock.newLine = lineUpdates.newLine;
                startBlock.oldLine = lineUpdates.oldLine;
            }
            else
            {
                // We use (newLine == oldLine) entry in LineUpdates as "end line" marker for moved region.
                if (startBlock.newLine != empty)
                    srcBlockUpdates[updateFileData.first].emplace_back(startBlock.newLine + 1, startBlock.oldLine + 1, lineUpdates.oldLine - 1 - startBlock.oldLine);

                startBlock.newLine = empty;
            }
        }
        // In case this is last method in source file, Roslyn don't provide "end line" in LineUpdates, use max source line as "end line".
        if (startBlock.newLine != empty)
            srcBlockUpdates[updateFileData.first].emplace_back(startBlock.newLine + 1, startBlock.oldLine + 1, std::numeric_limits<int32_t>::max());
    }

    return S_OK;
}

HRESULT ModulesSources::ApplyPdbDeltaAndLineUpdates(Modules *pModules, ICorDebugModule *pModule, bool needJMC, const std::string &deltaPDB,
                                                    const std::string &lineUpdates, std::unordered_set<mdMethodDef> &methodTokens)
{
    HRESULT Status;
    CORDB_ADDRESS modAddress;
    IfFailRet(pModule->GetBaseAddress(&modAddress));

    return pModules->GetModuleInfo(modAddress, [&](ModuleInfo &mdInfo) -> HRESULT
    {
        if (mdInfo.m_symbolReaderHandles.empty())
            return E_FAIL; // Deltas could be applied for already loaded modules with PDB only.

        PVOID pSymbolReaderHandle = nullptr;
        IfFailRet(Interop::LoadDeltaPdb(deltaPDB, &pSymbolReaderHandle, methodTokens));
        // Note, even if methodTokens is empty, pSymbolReaderHandle must be added into vector (we use indexes that correspond to il/metadata apply number + will care about release it in proper way).
        mdInfo.m_symbolReaderHandles.emplace_back(pSymbolReaderHandle);

        src_block_updates_t srcBlockUpdates;
        IfFailRet(LoadLineUpdatesFile(this, lineUpdates, srcBlockUpdates));

        if (methodTokens.empty() && srcBlockUpdates.empty())
            return S_OK;

        if (needJMC && !methodTokens.empty())
            DisableJMCByAttributes(pModule, methodTokens);

        ToRelease<IUnknown> pMDUnknown;
        IfFailRet(pModule->GetMetaDataInterface(IID_IMetaDataImport, &pMDUnknown));
        ToRelease<IMetaDataImport> pMDImport;
        IfFailRet(pMDUnknown->QueryInterface(IID_IMetaDataImport, (LPVOID*) &pMDImport));

        return UpdateSourcesCodeLinesForModule(pModule, pMDImport, methodTokens, srcBlockUpdates, mdInfo);
    });
}

HRESULT ModulesSources::GetSourceFullPathByIndex(unsigned index, std::string &fullPath)
{
    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    if (m_sourceIndexToPath.size() <= index)
        return E_FAIL;

#ifndef _WIN32
    fullPath = m_sourceIndexToPath[index];
#else
    fullPath = m_sourceIndexToInitialFullPath[index];
#endif

    return S_OK;
}

HRESULT ModulesSources::GetIndexBySourceFullPath(std::string fullPath, unsigned &index)
{
#ifdef WIN32
    HRESULT Status;
    IfFailRet(Interop::StringToUpper(fullPath));
#endif

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);

    auto findIndex = m_sourcePathToIndex.find(fullPath);
    if (findIndex == m_sourcePathToIndex.end())
        return E_FAIL;

    index = findIndex->second;
    return S_OK;
}

void ModulesSources::FindFileNames(Utility::string_view pattern, unsigned limit, std::function<void(const char *)> cb)
{
#ifdef WIN32
    std::string uppercase {pattern};
    if (FAILED(Interop::StringToUpper(uppercase)))
        return;
    pattern = uppercase;
#endif

    auto check = [&](const std::string& str)
    {
        if (limit == 0)
            return false;

        auto pos = str.find(pattern);
        if (pos != std::string::npos && (pos == 0 || str[pos-1] == '/' || str[pos-1] == '\\'))
        {
            limit--;
#ifndef _WIN32
            cb(str.c_str());
#else
            auto it = m_sourcePathToIndex.find(str);
            cb (it != m_sourcePathToIndex.end() ? m_sourceIndexToInitialFullPath[it->second].c_str() : str.c_str());
#endif
        }

        return true;
    };

    std::lock_guard<std::mutex> lock(m_sourcesInfoMutex);
    for (const auto &pair : m_sourceNameToFullPathsIndexes)
    {
        LOGD("first '%s'", pair.first.c_str());
        if (!check(pair.first))
            return;

        for (const unsigned fileIndex : pair.second)
        {
            LOGD("second '%s'", m_sourceIndexToPath[fileIndex].c_str());
            if (!check(m_sourceIndexToPath[fileIndex]))
                return;
        }
    }
}

} // namespace netcoredbg
