// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>
#include "interfaces/types.h"
#include "metadata/modules_app_update.h"
#include "metadata/modules_sources.h"
#include "utils/string_view.h"
#include "utils/torelease.h"
#include "utils/utf.h"

namespace netcoredbg
{

HRESULT GetModuleId(ICorDebugModule *pModule, std::string &id);
std::string GetModuleFileName(ICorDebugModule *pModule);
HRESULT IsModuleHaveSameName(ICorDebugModule *pModule, const std::string &Name, bool isFullPath);

class Modules
{
public:

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

    struct SequencePoint {
        int32_t startLine;
        int32_t startColumn;
        int32_t endLine;
        int32_t endColumn;
        int32_t offset;
        std::string document;
    };

    HRESULT ResolveBreakpoint(
        /*in*/ CORDB_ADDRESS modAddress,
        /*in*/ std::string filename,
        /*out*/ unsigned &fullname_index,
        /*in*/ int sourceLine,
        /*out*/ std::vector<ModulesSources::resolved_bp_t> &resolvedPoints);

    HRESULT GetSourceFullPathByIndex(unsigned index, std::string &fullPath);
    HRESULT GetIndexBySourceFullPath(std::string fullPath, unsigned &index);
    HRESULT ApplyPdbDeltaAndLineUpdates(ICorDebugModule *pModule, bool needJMC, const std::string &deltaPDB,
                                        const std::string &lineUpdates, std::unordered_set<mdMethodDef> &methodTokens);

    HRESULT GetModuleWithName(const std::string &name, ICorDebugModule **ppModule, bool onlyWithPDB = false);

    void CopyModulesUpdateHandlerTypes(std::vector<ToRelease<ICorDebugType>> &modulesUpdateHandlerTypes);

    typedef std::function<HRESULT(ModuleInfo &)> ModuleInfoCallback;
    HRESULT GetModuleInfo(CORDB_ADDRESS modAddress, ModuleInfoCallback cb);
    HRESULT GetModuleInfo(CORDB_ADDRESS modAddress, ModuleInfo **ppmdInfo);

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

    void FindFileNames(Utility::string_view pattern, unsigned limit, std::function<void(const char *)> cb);
    void FindFunctions(Utility::string_view pattern, unsigned limit, std::function<void(const char *)> cb);
    HRESULT GetSource(ICorDebugModule *pModule, const std::string &sourcePath, char** fileBuf, int* fileLen);

private:

    std::mutex m_modulesInfoMutex;
    std::unordered_map<CORDB_ADDRESS, ModuleInfo> m_modulesInfo;
    ModulesAppUpdate m_modulesAppUpdate;

    // Note, m_modulesSources have its own mutex for private data state sync.
    ModulesSources m_modulesSources;

    HRESULT GetSequencePointByILOffset(
        PVOID pSymbolReaderHandle,
        mdMethodDef methodToken,
        ULONG32 ilOffset,
        SequencePoint *sequencePoint);

};

} // namespace netcoredbg
