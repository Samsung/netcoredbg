// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_map>
#include <set>
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
HRESULT DisableJMCByAttributes(ICorDebugModule *pModule, PVOID pSymbolReaderHandle);

struct method_input_data_t
{
    mdMethodDef methodDef;
    int32_t startLine; // first segment/method SequencePoint's startLine
    int32_t endLine; // last segment/method SequencePoint's endLine
    int32_t startColumn; // first segment/method SequencePoint's startColumn
    int32_t endColumn; // last segment/method SequencePoint's endColumn

    method_input_data_t(mdMethodDef methodDef_, int32_t startLine_, int32_t endLine_, int32_t startColumn_, int32_t endColumn_) :
        methodDef(methodDef_),
        startLine(startLine_),
        endLine(endLine_),
        startColumn(startColumn_),
        endColumn(endColumn_)
    {}

    bool operator < (const method_input_data_t &other) const
    {
        return endLine < other.endLine || (endLine == other.endLine && endColumn < other.endColumn);
    }
    bool NestedInto(const method_input_data_t &other) const
    {
        assert(startLine != other.startLine || startColumn != other.startColumn);
        assert(endLine != other.endLine || endColumn != other.endColumn);

        return (startLine > other.startLine || (startLine == other.startLine && startColumn > other.startColumn)) &&
            (endLine < other.endLine || (endLine == other.endLine && endColumn < other.endColumn));
    }
};

struct method_data_t
{
    mdMethodDef methodDef;
    uint32_t startLine; // first segment/method SequencePoint's startLine
    uint32_t endLine; // last segment/method SequencePoint's endLine

    method_data_t() = default;
    method_data_t(mdMethodDef methodDef_, uint32_t startLine_, uint32_t endLine_) :
        methodDef(methodDef_),
        startLine(startLine_),
        endLine(endLine_)
    {}
    method_data_t(const method_input_data_t &other) :
        methodDef(other.methodDef),
        startLine(other.startLine),
        endLine(other.endLine)
    {
        assert(other.startLine >= 0);
        assert(other.endLine >= 0);
    }

    bool operator < (const uint32_t lineNum) const
    {
        return endLine < lineNum;
    }

    bool operator == (const method_data_t &other) const
    {
        return methodDef == other.methodDef &&  startLine == other.startLine &&  endLine == other.endLine;
    }
};

struct method_data_t_hash
{
    size_t operator()(const method_data_t &p) const
    {
        return p.methodDef + p.startLine * 100 + p.endLine * 1000;
    }
};


HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id);
std::string GetModuleFileName(ICorDebugModule *pModule);
HRESULT IsModuleHaveSameName(ICorDebugModule *pModule, const std::string &Name, bool isFullPath);

class Modules
{
    struct ModuleInfo
    {
        PVOID m_symbolReaderHandle = nullptr;
        ToRelease<ICorDebugModule> m_iCorModule;

        ModuleInfo(PVOID Handle, ICorDebugModule *Module) :
            m_symbolReaderHandle(Handle),
            m_iCorModule(Module)
        {}

        ModuleInfo(ModuleInfo&& other) noexcept :
            m_symbolReaderHandle(other.m_symbolReaderHandle),
            m_iCorModule(std::move(other.m_iCorModule))
        {
            other.m_symbolReaderHandle = nullptr;
        }
        ModuleInfo(const ModuleInfo&) = delete;
        ModuleInfo& operator=(ModuleInfo&&) = delete;
        ModuleInfo& operator=(const ModuleInfo&) = delete;
        ~ModuleInfo() noexcept;
    };

    std::mutex m_modulesInfoMutex;
    std::unordered_map<CORDB_ADDRESS, ModuleInfo> m_modulesInfo;

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

    HRESULT FillSourcesCodeLinesForModule(ICorDebugModule *pModule, IMetaDataImport *pMDImport, PVOID pSymbolReaderHandle);
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

    HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule);

    HRESULT GetFrameILAndSequencePoint(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        SequencePoint &sequencePoint);

    HRESULT GetFrameILAndNextUserCodeILOffset(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        ULONG32 &ilCloseOffset,
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
        bool needJMC);

    void CleanupAllModules();

    HRESULT GetFrameNamedLocalVariable(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG localIndex,
        WSTRING &localName,
        ULONG32 *pIlStart,
        ULONG32 *pIlEnd);

    HRESULT GetHoistedLocalScopes(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        PVOID *data,
        int32_t &hoistedLocalScopesCount);

    HRESULT GetNextSequencePointInMethod(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG32 ilOffset,
        ULONG32 &ilCloseOffset,
        bool *noUserCodeFound = nullptr);

    HRESULT GetSequencePointByILOffset(
        PVOID pSymbolReaderHandle,
        mdMethodDef methodToken,
        ULONG32 ilOffset,
        SequencePoint *sequencePoint);

    HRESULT GetSequencePointByILOffset(
        CORDB_ADDRESS modAddress,
        mdMethodDef methodToken,
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

    struct AsyncMethodInfo
    {
        std::vector<AwaitInfo> awaits;
        // Part of NotifyDebuggerOfWaitCompletion magic, see ManagedDebugger::SetupAsyncStep().
        ULONG32 lastIlOffset;

        AsyncMethodInfo() :
            awaits(), lastIlOffset(0)
        {};
    };

    bool IsMethodHaveAwait(CORDB_ADDRESS modAddress, mdMethodDef methodToken);
    bool FindNextAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 ipOffset, AwaitInfo **awaitInfo);
    bool FindLastIlOffsetAwaitInfo(CORDB_ADDRESS modAddress, mdMethodDef methodToken, ULONG32 &lastIlOffset);

private:

    struct PairHash
    {
        template <class T1, class T2>
        size_t operator()(const std::pair<T1, T2> &pair) const
        {
            return std::hash<T1>{}(pair.first) ^ std::hash<T2>{}(pair.second);
        }
    };
    // All async methods stepping information for all loaded (with symbols) modules.
    std::unordered_map<std::pair<CORDB_ADDRESS, mdMethodDef>, AsyncMethodInfo, PairHash> m_asyncMethodsSteppingInfo;
    std::mutex m_asyncMethodsSteppingInfoMutex;
    HRESULT FillAsyncMethodsSteppingInfo(ICorDebugModule *pModule, PVOID pSymbolReaderHandle);
};

} // namespace netcoredbg
