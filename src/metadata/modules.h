// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#pragma warning (disable:4068)  // Visual Studio should ignore GCC pragmas
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <cor.h>
#pragma GCC diagnostic pop

#include <cordebug.h>

#include <functional>
#include <unordered_map>
#include <map>
#include <list>
#include <mutex>
#include <memory>

#include "protocols/protocol.h"
#include "torelease.h"

namespace netcoredbg
{

class SymbolReader;
typedef std::function<HRESULT(ICorDebugModule *, mdMethodDef &)> ResolveFunctionBreakpointCallback;

class Modules
{
    struct ModuleInfo
    {
        std::unique_ptr<SymbolReader> symbols;
        ToRelease<ICorDebugModule> module;

        ModuleInfo(ModuleInfo &&) = default;
        ModuleInfo(const ModuleInfo &) = delete;
    };

    std::mutex m_modulesInfoMutex;
    std::unordered_map<CORDB_ADDRESS, ModuleInfo> m_modulesInfo;

    static HRESULT SetJMCFromAttributes(ICorDebugModule *pModule, SymbolReader *symbolReader);
    static HRESULT ResolveMethodInModule(
        ICorDebugModule *pModule,
        const std::string &funcName,
        ResolveFunctionBreakpointCallback cb);
    static bool IsTargetFunction(const std::vector<std::string> &fullName, const std::vector<std::string> &targetName);

    // map of source full path to all sequence point's startLine and endLine in this source file,
    // need it in order to resolve requested breakpoint line number to proper line number related to executable code
    std::unordered_map<std::string, std::map<int32_t, std::tuple<int32_t, int32_t> > > m_sourcesCodeLines;
    // map of source file name to list of source full paths from loaded assemblies,
    // need it order to resolve source full path by requested breakpoint relative source path
    std::unordered_map<std::string, std::list<std::string> > m_sourcesFullPaths;
    HRESULT Modules::FillSourcesCodeLinesForModule(IMetaDataImport *pMDImport, SymbolReader *symbolReader);
    HRESULT Modules::ResolveRelativeSourceFileName(std::string &filename);
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

    HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule);

    HRESULT GetFrameILAndSequencePoint(
        ICorDebugFrame *pFrame,
        ULONG32 &ilOffset,
        SequencePoint &sequencePoint);

    HRESULT GetLocationInModule(
        ICorDebugModule *pModule,
        std::string filename,
        ULONG linenum,
        ULONG32 &ilOffset,
        mdMethodDef &methodToken);

    HRESULT GetLocationInAny(
        std::string filename,
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

    HRESULT GetSequencePointByILOffset(
        SymbolReader *symbols,
        mdMethodDef methodToken,
        ULONG32 &ilOffset,
        SequencePoint *sequencePoint);

    HRESULT ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb);

    HRESULT ResolveBreakpointFileAndLine(std::string &filename, int &linenum, int &endLine);
};

} // namespace netcoredbg
