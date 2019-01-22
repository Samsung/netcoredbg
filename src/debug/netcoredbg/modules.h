// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <cor.h>
#include <cordebug.h>

#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "protocol.h"
#include "torelease.h"


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

    static bool ShouldLoadSymbolsForModule(const std::string &moduleName);
    static HRESULT SetJMCFromAttributes(ICorDebugModule *pModule, SymbolReader *symbolReader);
    static HRESULT ResolveMethodInModule(
        ICorDebugModule *pModule,
        const std::string &funcName,
        ResolveFunctionBreakpointCallback cb);

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
        mdMethodDef &methodToken,
        std::string &fullname);

    HRESULT GetLocationInAny(
        std::string filename,
        ULONG linenum,
        ULONG32 &ilOffset,
        mdMethodDef &methodToken,
        std::string &fullname,
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
        Module &module);

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

    HRESULT ForEachModule(std::function<HRESULT(ICorDebugModule *pModule)> cb);
};
