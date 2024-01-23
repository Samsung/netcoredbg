// Copyright (c) 2022 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoint_hotreload.h"
#include "debugger/breakpointutils.h"
#include "debugger/hotreloadhelpers.h"
#include "debugger/evalhelpers.h"
#include "metadata/modules.h"

namespace netcoredbg
{

HRESULT HotReloadBreakpoint::SetHotReloadBreakpoint(const std::string &updatedDLL, const std::unordered_set<mdTypeDef> &updatedTypeTokens)
{
#ifdef NCDB_DOTNET_STARTUP_HOOK

    std::lock_guard<std::mutex> lock(m_reloadMutex);

    if (!m_iCorFunc)
        return E_FAIL;

    if (m_iCorFuncBreakpoint)
    {
        // Case with several deltas applyed during `pause`.
        // TODO (?) support several DLLs update.
        assert(m_updatedDLL == updatedDLL);
        for (const auto &typeTokens : updatedTypeTokens)
        {
            m_updatedTypeTokens.insert(typeTokens);
        }
        return S_OK;
    }

    HRESULT Status;
    IfFailRet(m_iCorFunc->CreateBreakpoint(&m_iCorFuncBreakpoint));
    if (FAILED(Status = m_iCorFuncBreakpoint->Activate(TRUE)))
    {
        m_iCorFuncBreakpoint.Free();
        return Status;
    }

    m_updatedDLL = updatedDLL;
    m_updatedTypeTokens = updatedTypeTokens;

    return S_OK;

#else // NCDB_DOTNET_STARTUP_HOOK

    return E_NOTIMPL;

#endif // NCDB_DOTNET_STARTUP_HOOK
}

HRESULT HotReloadBreakpoint::ManagedCallbackLoadModuleAll(ICorDebugModule *pModule)
{
#ifdef NCDB_DOTNET_STARTUP_HOOK

    static std::string dllName(NCDB_DOTNET_STARTUP_HOOK);

    std::lock_guard<std::mutex> lock(m_reloadMutex);

    if (dllName != GetModuleFileName(pModule))
        return S_OK;

    HRESULT Status;
    static const WCHAR className[] = W("StartupHook");
    static const WCHAR methodName[] = W("ncdbfunc");
    IfFailRet(FindFunction(pModule, className, methodName, &m_iCorFunc));

#endif // NCDB_DOTNET_STARTUP_HOOK

    return S_OK;
}

HRESULT HotReloadBreakpoint::CheckApplicationReload(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint)
{
    std::lock_guard<std::mutex> lock(m_reloadMutex);

    if (!m_iCorFuncBreakpoint)
        return S_FALSE; // S_FALSE - no error, but not affect on callback

    HRESULT Status;
    HRESULT ReturnStatus;
    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    IfFailRet(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID*) &pFunctionBreakpoint));
    IfFailRet(BreakpointUtils::IsSameFunctionBreakpoint(pFunctionBreakpoint, m_iCorFuncBreakpoint));
    ReturnStatus = Status; // In case S_FALSE, probably, we stopped on another breakpoint, need check this and emit event.

    IfFailRet(HotReloadHelpers::UpdateApplication(pThread, m_sharedModules.get(), m_sharedEvaluator.get(), m_sharedEvalHelpers.get(),
                                                  m_updatedDLL, m_updatedTypeTokens));

    Clear();

    return ReturnStatus;
}

void HotReloadBreakpoint::CheckApplicationReload(ICorDebugThread *pThread)
{
    std::lock_guard<std::mutex> lock(m_reloadMutex);

    if (!m_iCorFuncBreakpoint)
        return;

    HotReloadHelpers::UpdateApplication(pThread, m_sharedModules.get(), m_sharedEvaluator.get(), m_sharedEvalHelpers.get(),
                                        m_updatedDLL, m_updatedTypeTokens);

    Clear();
}

void HotReloadBreakpoint::Delete()
{
    std::lock_guard<std::mutex> lock(m_reloadMutex);

    if (!m_iCorFuncBreakpoint)
        return;

    Clear();
}

// Caller must care about m_reloadMutex.
void HotReloadBreakpoint::Clear()
{
    m_iCorFuncBreakpoint->Activate(FALSE);
    m_iCorFuncBreakpoint.Free();
    m_updatedDLL.clear();
    m_updatedTypeTokens.clear();
}

} // namespace netcoredbg
