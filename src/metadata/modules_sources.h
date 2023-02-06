// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <set>
#include <mutex>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include "utils/string_view.h"
#include "utils/torelease.h"


namespace netcoredbg
{

typedef std::function<HRESULT(ICorDebugModule *, mdMethodDef &)> ResolveFuncBreakpointCallback;

struct method_data_t
{
    mdMethodDef methodDef;
    int32_t startLine; // first segment/method SequencePoint's startLine
    int32_t endLine; // last segment/method SequencePoint's endLine
    int32_t startColumn; // first segment/method SequencePoint's startColumn
    int32_t endColumn; // last segment/method SequencePoint's endColumn

    method_data_t() :
        methodDef(0),
        startLine(0),
        endLine(0),
        startColumn(0),
        endColumn(0)
    {}

    method_data_t(mdMethodDef methodDef_, int32_t startLine_, int32_t endLine_, int32_t startColumn_, int32_t endColumn_) :
        methodDef(methodDef_),
        startLine(startLine_),
        endLine(endLine_),
        startColumn(startColumn_),
        endColumn(endColumn_)
    {}

    bool operator < (const method_data_t &other) const
    {
        return endLine < other.endLine || (endLine == other.endLine && endColumn < other.endColumn);
    }

    bool operator < (const int32_t lineNum) const
    {
        return endLine < lineNum;
    }

    bool operator == (const method_data_t &other) const
    {
        return methodDef == other.methodDef &&
               startLine == other.startLine && endLine == other.endLine &&
               startColumn == other.startColumn && endColumn == other.endColumn;
    }

    bool NestedInto(const method_data_t &other) const
    {
        assert(startLine != other.startLine || startColumn != other.startColumn);
        assert(endLine != other.endLine || endColumn != other.endColumn);

        return (startLine > other.startLine || (startLine == other.startLine && startColumn > other.startColumn)) &&
            (endLine < other.endLine || (endLine == other.endLine && endColumn < other.endColumn));
    }
};

struct method_data_t_hash
{
    size_t operator()(const method_data_t &p) const
    {
        return p.methodDef + (uint32_t)p.startLine * 100 + (uint32_t)p.endLine * 1000;
    }
};

struct block_update_t
{
    int32_t newLine;
    int32_t oldLine;
    int32_t endLineOffset;
    block_update_t(int32_t newLine_, int32_t oldLine_, int32_t endLineOffset_) :
        newLine(newLine_), oldLine(oldLine_), endLineOffset(endLineOffset_)
    {}
};
typedef std::unordered_map<unsigned /*source fullPathIndex*/, std::vector<block_update_t>> src_block_updates_t;

struct file_block_update_t
{
    unsigned fullPathIndex;
    int32_t newLine;
    int32_t oldLine;
    int32_t endLineOffset;
    file_block_update_t(unsigned fullPathIndex_, int32_t newLine_, int32_t oldLine_, int32_t endLineOffset_) :
        fullPathIndex(fullPathIndex_), newLine(newLine_), oldLine(oldLine_), endLineOffset(endLineOffset_)
    {}
};
typedef std::unordered_map<mdMethodDef, std::vector<file_block_update_t>> method_block_updates_t;

template <class T>
void LineUpdatesForwardCorrection(unsigned fullPathIndex, mdMethodDef methodToken, method_block_updates_t &methodBlockUpdates, T &block)
{
    auto findSourceUpdate = methodBlockUpdates.find(methodToken);
    if (findSourceUpdate == methodBlockUpdates.end())
        return;

    for (const auto &entry : findSourceUpdate->second)
    {
        if (entry.fullPathIndex != fullPathIndex ||
            entry.oldLine > block.startLine ||
            entry.oldLine + entry.endLineOffset < block.startLine)
            continue;

        int32_t offset = entry.newLine - entry.oldLine;
        block.startLine += offset;
        block.endLine += offset;
        break;
    }
}

class Modules;
struct ModuleInfo;

class ModulesSources
{
public:

    struct resolved_bp_t
    {
        int32_t startLine;
        int32_t endLine;
        uint32_t ilOffset;
        uint32_t methodToken;
        ToRelease<ICorDebugModule> iCorModule;

        resolved_bp_t(int32_t startLine_, int32_t endLine_, uint32_t ilOffset_, uint32_t methodToken_, ICorDebugModule *pModule) :
            startLine(startLine_),
            endLine(endLine_),
            ilOffset(ilOffset_),
            methodToken(methodToken_),
            iCorModule(pModule)
        {}
    };

    HRESULT ResolveBreakpoint(
        /*in*/ Modules *pModules,
        /*in*/ CORDB_ADDRESS modAddress,
        /*in*/ std::string filename,
        /*out*/ unsigned &fullname_index,
        /*in*/ int sourceLine,
        /*out*/ std::vector<resolved_bp_t> &resolvedPoints);

    HRESULT FillSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle);
    HRESULT GetSourceFullPathByIndex(unsigned index, std::string &fullPath);
    HRESULT GetIndexBySourceFullPath(std::string fullPath, unsigned &index);
    HRESULT ApplyPdbDeltaAndLineUpdates(Modules *pModules, ICorDebugModule *pModule, bool needJMC, const std::string &deltaPDB,
                                        const std::string &lineUpdates, std::unordered_set<mdMethodDef> &methodTokens);

    void FindFileNames(Utility::string_view pattern, unsigned limit, std::function<void(const char *)> cb);

private:

    struct FileMethodsData
    {
        CORDB_ADDRESS modAddress = 0;
        // properly ordered on each nested level arrays of methods data
        std::vector<std::vector<method_data_t>> methodsData;
        // mapping method's data to array of tokens, that also represent same code
        // aimed to resolve all methods token for constructor's segment, since it could be part of multiple constructors
        std::unordered_map<method_data_t, std::vector<mdMethodDef>, method_data_t_hash> multiMethodsData;
    };

    // Note, breakpoints setup and ran debuggee's process could be in the same time.
    std::mutex m_sourcesInfoMutex;
    // Note, we only add to m_sourceIndexToPath/m_sourcePathToIndex/m_sourceIndexToInitialFullPath, "size()" used as index in map at new element add.
    // m_sourceIndexToPath - mapping index to full path
    std::vector<std::string> m_sourceIndexToPath;
    // m_sourcePathToIndex - mapping full path to index
    std::unordered_map<std::string, unsigned> m_sourcePathToIndex;
    // m_sourceNameToFullPathsIndexes - mapping file name to set of paths with this file name
    std::unordered_map<std::string, std::set<unsigned>> m_sourceNameToFullPathsIndexes;
    // m_sourcesMethodsData - all methods data indexed by full path, second vector hold data with same full path for different modules,
    //                        since we may have modules with same source full path
    std::vector<std::vector<FileMethodsData>> m_sourcesMethodsData;

    HRESULT GetFullPathIndex(BSTR document, unsigned &fullPathIndex);
    HRESULT UpdateSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, std::unordered_set<mdMethodDef> methodTokens,
                                            src_block_updates_t &blockUpdates, ModuleInfo &mdInfo);
    HRESULT ResolveRelativeSourceFileName(std::string &filename);
    HRESULT LineUpdatesForMethodData(ICorDebugModule *pModule, unsigned fullPathIndex, method_data_t &methodData,
                                     const std::vector<block_update_t> &blockUpdate, ModuleInfo &mdInfo);

#ifdef WIN32
    // on Windows OS, all files names converted to uppercase in containers above, but this vector hold initial full path names
    std::vector<std::string> m_sourceIndexToInitialFullPath;
#endif

};

} // namespace netcoredbg
