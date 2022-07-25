// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_map>
#include <set>
#include <list>
#include <mutex>
#include <memory>
#include "interfaces/types.h"
#include "utils/string_view.h"
#include "utils/torelease.h"
#include "utils/utf.h"

namespace netcoredbg
{
using Utility::string_view;

typedef std::function<HRESULT(ICorDebugModule *, mdMethodDef &)> ResolveFuncBreakpointCallback;

struct DebuggerAttribute
{
    static const char NonUserCode[];
    static const char StepThrough[];
    static const char Hidden[];
};

bool HasAttribute(IMetaDataImport *pMD, mdToken tok, const char *attrName);
bool HasAttribute(IMetaDataImport *pMD, mdToken tok, std::vector<std::string> &attrNames);
HRESULT DisableJMCByAttributes(ICorDebugModule *pModule);
HRESULT DisableJMCByAttributes(ICorDebugModule *pModule, const std::unordered_set<mdMethodDef> &methodTokens);

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


HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id);
std::string GetModuleFileName(ICorDebugModule *pModule);
HRESULT IsModuleHaveSameName(ICorDebugModule *pModule, const std::string &Name, bool isFullPath);

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

class Modules
{
    struct ModuleInfo
    {
        std::vector<PVOID> m_symbolReaderHandles;
        ToRelease<ICorDebugModule> m_iCorModule;
        // Cache for LineUpdates data for all methods in this module (Hot Reload related).
        method_block_updates_t m_methodBlockUpdates;

        ModuleInfo(PVOID Handle, ICorDebugModule *Module) :
            m_iCorModule(Module)
        {
            if (Handle == nullptr)
                return;

            m_symbolReaderHandles.reserve(1);
            m_symbolReaderHandles.emplace_back(Handle);
        }

        ModuleInfo(ModuleInfo&& other) noexcept :
            m_symbolReaderHandles(std::move(other.m_symbolReaderHandles)),
            m_iCorModule(std::move(other.m_iCorModule))
        {
        }
        ModuleInfo(const ModuleInfo&) = delete;
        ModuleInfo& operator=(ModuleInfo&&) = delete;
        ModuleInfo& operator=(const ModuleInfo&) = delete;
        ~ModuleInfo() noexcept;
    };

    std::mutex m_modulesInfoMutex;
    std::unordered_map<CORDB_ADDRESS, ModuleInfo> m_modulesInfo;
    // Must care about topological sort during ClearCache() and UpdateApplication() methods calls at Hot Reload.
    std::list<ToRelease<ICorDebugType>> m_modulesUpdateHandlerTypes;

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
    HRESULT FillSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle);
    HRESULT UpdateSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, std::unordered_set<mdMethodDef> methodTokens,
                                            src_block_updates_t &blockUpdates, PVOID pSymbolReaderHandle, method_block_updates_t &methodBlockUpdates);
    HRESULT ResolveRelativeSourceFileName(std::string &filename);

#ifdef WIN32
    // on Windows OS, all files names converted to uppercase in containers above, but this vector hold initial full path names
    std::vector<std::string> m_sourceIndexToInitialFullPath;
#endif

public:
    struct SequencePoint {
        int32_t startLine;
        int32_t startColumn;
        int32_t endLine;
        int32_t endColumn;
        int32_t offset;
        std::string document;
    };

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
        /*in*/ CORDB_ADDRESS modAddress,
        /*in*/ std::string filename,
        /*out*/ unsigned &fullname_index,
        /*in*/ int sourceLine,
        /*out*/ std::vector<resolved_bp_t> &resolvedPoints);

    HRESULT GetSourceFullPathByIndex(unsigned index, std::string &fullPath);
    HRESULT GetIndexBySourceFullPath(std::string fullPath, unsigned &index);
    HRESULT ApplyPdbDeltaAndLineUpdates(ICorDebugModule *pModule, bool needJMC, const std::string &deltaPDB,
                                        const std::string &lineUpdates, std::unordered_set<mdMethodDef> &methodTokens);

    HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule, bool onlyWithPDB = false);

    void CopyModulesUpdateHandlerTypes(std::vector<ToRelease<ICorDebugType>> &modulesUpdateHandlerTypes);

    HRESULT GetFrameILAndSequencePoint(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        SequencePoint &sequencePoint);

    HRESULT GetFrameILAndNextUserCodeILOffset(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        ULONG32 &ilNextOffset,
        bool *noUserCodeFound);

    HRESULT ResolveFuncBreakpointInAny(
        const std::string &module,
        bool &module_checked,
        const std::string &funcname,
        ResolveFuncBreakpointCallback cb);

    HRESULT ResolveFuncBreakpointInModule(
        ICorDebugModule *pModule,
        const std::string &module,
        bool &module_checked,
        std::string &funcname,
        ResolveFuncBreakpointCallback cb);

    HRESULT GetStepRangeFromCurrentIP(
        ICorDebugThread *pThread,
        COR_DEBUG_STEP_RANGE *range);

    HRESULT TryLoadModuleSymbols(
        ICorDebugModule *pModule,
        Module &module,
        bool needJMC,
        bool needHotReload,
        std::string &outputText);

    void CleanupAllModules();

    HRESULT GetFrameNamedLocalVariable(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG32 methodVersion,
        ULONG localIndex,
        WSTRING &localName,
        ULONG32 *pIlStart,
        ULONG32 *pIlEnd);

    HRESULT GetHoistedLocalScopes(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG32 methodVersion,
        PVOID *data,
        int32_t &hoistedLocalScopesCount);

    HRESULT GetNextUserCodeILOffsetInMethod(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG32 methodVersion,
        ULONG32 ilOffset,
        ULONG32 &ilNextOffset,
        bool *noUserCodeFound = nullptr);

    HRESULT GetSequencePointByILOffset(
        CORDB_ADDRESS modAddress,
        mdMethodDef methodToken,
        ULONG32 methodVersion,
        ULONG32 ilOffset,
        SequencePoint &sequencePoint);

    HRESULT ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb);

    void FindFileNames(string_view pattern, unsigned limit, std::function<void(const char *)> cb);
    void FindFunctions(string_view pattern, unsigned limit, std::function<void(const char *)> cb);
    HRESULT GetSource(ICorDebugModule *pModule, const std::string &sourcePath, char** fileBuf, int* fileLen);

    struct AwaitInfo
    {
        uint32_t yield_offset;
        uint32_t resume_offset;

        AwaitInfo() :
            yield_offset(0), resume_offset(0)
        {};
        AwaitInfo(uint32_t offset1, uint32_t offset2) :
            yield_offset(offset1), resume_offset(offset2)
        {};
    };

    bool IsMethodHaveAwait(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion);
    bool FindNextAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 ipOffset, AwaitInfo **awaitInfo);
    bool FindLastIlOffsetAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion, ULONG32 &lastIlOffset);

private:

    HRESULT GetSequencePointByILOffset(
        PVOID pSymbolReaderHandle,
        mdMethodDef methodToken,
        ULONG32 ilOffset,
        SequencePoint *sequencePoint);

    struct AsyncMethodInfo
    {
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 methodVersion;

        std::vector<AwaitInfo> awaits;
        // Part of NotifyDebuggerOfWaitCompletion magic, see ManagedDebugger::SetupAsyncStep().
        ULONG32 lastIlOffset;

        AsyncMethodInfo() :
            modAddress(0), methodToken(mdMethodDefNil), methodVersion(0), awaits(), lastIlOffset(0)
        {};
    };

    AsyncMethodInfo asyncMethodSteppingInfo;
    std::mutex m_asyncMethodSteppingInfoMutex;
    // Note, result stored into asyncMethodSteppingInfo.
    HRESULT GetAsyncMethodSteppingInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 methodVersion);
};

} // namespace netcoredbg
