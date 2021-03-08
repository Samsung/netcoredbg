// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <memory>

#include "string_view.h"
#include "protocols/protocol.h"
#include "string_view.h"
#include "torelease.h"

namespace netcoredbg
{
using Utility::string_view;

using Utility::string_view;

class ManagedPart;
typedef std::function<HRESULT(ICorDebugModule *, mdMethodDef &)> ResolveFunctionBreakpointCallback;

HRESULT GetModuleName(ICorDebugThread *pThread, std::string &module);

class Modules
{
    struct ModuleInfo
    {
        std::unique_ptr<ManagedPart> managedPart;
        ToRelease<ICorDebugModule> module;

        ModuleInfo(ModuleInfo &&) = default;
        ModuleInfo(const ModuleInfo &) = delete;
    };

    std::mutex m_modulesInfoMutex;
    std::unordered_map<CORDB_ADDRESS, ModuleInfo> m_modulesInfo;

    static HRESULT SetJMCFromAttributes(ICorDebugModule *pModule, ManagedPart *managedPart);

    static HRESULT ResolveMethodInModule(
        ICorDebugModule *pModule,
        const std::string &funcName,
        ResolveFunctionBreakpointCallback cb);

    static HRESULT ForEachMethod(ICorDebugModule *pModule, std::function<bool(const std::string&, mdMethodDef&)>);

    static bool IsTargetFunction(const std::vector<std::string> &fullName, const std::vector<std::string> &targetName);

    // sync access to m_sourcesCodeLines, m_sourcesFullPaths and m_sourceCaseSensitiveFullPaths,
    // since breakpoint resolve and module load could be in the same time
    // (breakpoints setup with ran debuggee's process in the same time, for example)
    std::mutex m_sourcesInfoMutex;
    // map of source full path to all sequence point's startLine and endLine in this source file,
    // need it in order to resolve requested breakpoint line number to proper line number related to executable code
    std::unordered_map<std::string, std::map<int32_t, std::tuple<int32_t, int32_t> > > m_sourcesCodeLines;
    // map of source file name to list of source full paths from loaded assemblies,
    // need it order to resolve source full path by requested breakpoint relative source path
    std::unordered_map<std::string, std::set<std::string> > m_sourcesFullPaths;
    HRESULT FillSourcesCodeLinesForModule(IMetaDataImport *pMDImport, ManagedPart *managedPart);
    HRESULT ResolveRelativeSourceFileName(std::string &filename);
#ifdef WIN32
    // all internal breakpoint routine are case sensitive, proper source full name for Windows must be used (same as in module)
    std::unordered_map<std::string, std::string> m_sourceCaseSensitiveFullPaths;
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

    static HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id);
    static std::string GetModuleFileName(ICorDebugModule *pModule);

    // This function strips directory path from file name.
    static string_view GetFileName(string_view path);

    HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule);

    HRESULT GetFrameILAndSequencePoint(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        SequencePoint &sequencePoint);

    HRESULT GetLocationInModule(
        ICorDebugModule *pModule,
        const std::string& filename,
        ULONG linenum,
        ULONG32 &ilOffset,
        mdMethodDef &methodToken);

    HRESULT GetLocationInAny(
        const std::string& filename,
        ULONG linenum,
        ULONG32 &ilOffset,
        mdMethodDef &methodToken,
        ICorDebugModule **ppModule);

    HRESULT ResolveFunctionInAny(
        const std::string &module,
        const std::string &funcname,
        ResolveFunctionBreakpointCallback cb);

    HRESULT ResolveFunctionInModule(
        ICorDebugModule *pModule,
        const std::string &module,
        std::string &funcname,
        ResolveFunctionBreakpointCallback cb);

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
        ICorDebugILFrame *pILFrame,
        mdMethodDef methodToken,
        ULONG localIndex,
        std::string &paramName,
        ICorDebugValue** ppValue,
        ULONG32 *pIlStart,
        ULONG32 *pIlEnd);

    HRESULT Modules::GetNextSequencePointInMethod(
        ICorDebugModule *pModule,
        mdMethodDef methodToken,
        ULONG32 ilOffset,
        Modules::SequencePoint &sequencePoint);

    HRESULT GetSequencePointByILOffset(
        ManagedPart *managedPart,
        mdMethodDef methodToken,
        ULONG32 &ilOffset,
        SequencePoint *sequencePoint);

    HRESULT ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb);

    HRESULT ResolveBreakpointFileAndLine(std::string &filename, int &linenum, int &endLine);

    void FindFileNames(string_view pattern, unsigned limit, std::function<void(const char *)> cb);
    void FindFunctions(string_view pattern, unsigned limit, std::function<void(const char *)> cb);

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
        uint32_t catch_handler_offset;
        std::vector<AwaitInfo> awaits;
        // Part of NotifyDebuggerOfWaitCompletion magic, see ManagedDebugger::SetupAsyncStep().
        ULONG32 lastIlOffset;

        AsyncMethodInfo() :
            catch_handler_offset(0), awaits(), lastIlOffset(0)
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
    HRESULT FillAsyncMethodsSteppingInfo(ICorDebugModule *pModule, ManagedPart *managedPart);
};

} // namespace netcoredbg
