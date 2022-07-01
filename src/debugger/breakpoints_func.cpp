// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "debugger/breakpoints_func.h"
#include "debugger/breakpointutils.h"
#include "metadata/typeprinter.h"
#include "metadata/modules.h"
#include <sstream>
#include <unordered_set>
#include <algorithm>

namespace netcoredbg
{

void FuncBreakpoints::ManagedFuncBreakpoint::ToBreakpoint(Breakpoint &breakpoint) const
{
    breakpoint.id = this->id;
    breakpoint.verified = this->IsVerified();
    breakpoint.condition = this->condition;
    breakpoint.module = this->module;
    breakpoint.funcname = this->name;
    breakpoint.params = this->params;
}

void FuncBreakpoints::DeleteAll()
{
    m_breakpointsMutex.lock();
    m_funcBreakpoints.clear();
    m_breakpointsMutex.unlock();
}

HRESULT FuncBreakpoints::CheckBreakpointHit(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint)
{
    if (m_funcBreakpoints.empty())
        return S_FALSE; // Stopped at break, but no breakpoints.

    HRESULT Status;
    ToRelease<ICorDebugFunctionBreakpoint> pFunctionBreakpoint;
    IfFailRet(pBreakpoint->QueryInterface(IID_ICorDebugFunctionBreakpoint, (LPVOID *) &pFunctionBreakpoint));

    ToRelease<ICorDebugFrame> pFrame;
    IfFailRet(pThread->GetActiveFrame(&pFrame));
    if (pFrame == nullptr)
        return E_FAIL;

    ToRelease<ICorDebugILFrame> pILFrame;
    IfFailRet(pFrame->QueryInterface(IID_ICorDebugILFrame, (LPVOID *) &pILFrame));

    ToRelease<ICorDebugValueEnum> pParamEnum;
    IfFailRet(pILFrame->EnumerateArguments(&pParamEnum));
    ULONG cParams = 0;
    IfFailRet(pParamEnum->GetCount(&cParams));

    std::ostringstream ss;
    ss << "(";
    if (cParams > 0)
    {
        for (ULONG i = 0; i < cParams; ++i)
        {
            ToRelease<ICorDebugValue> pValue;
            ULONG cArgsFetched;
            if (FAILED(pParamEnum->Next(1, &pValue, &cArgsFetched)))
                continue;

            std::string param;
            IfFailRet(TypePrinter::GetTypeOfValue(pValue, param));
            if (i > 0)
                ss << ",";

            ss << param;
        }
    }
    ss << ")";
    std::string params = ss.str();

    // Note, since IsEnableByCondition() during eval execution could neutered frame, all frame-related calculation
    // must be done before enter into this cycles.
    for (auto &fb : m_funcBreakpoints)
    {
        ManagedFuncBreakpoint &fbp = fb.second;

        if (!fbp.enabled || (!fbp.params.empty() && params != fbp.params))
            continue;

        for (auto &funcBreakpoint : fbp.funcBreakpoints)
        {
            if (FAILED(BreakpointUtils::IsSameFunctionBreakpoint(pFunctionBreakpoint, funcBreakpoint.iCorFuncBreakpoint)) ||
                FAILED(BreakpointUtils::IsEnableByCondition(fbp.condition, m_sharedVariables.get(), pThread)))
                continue;
            
            ++fbp.times;
            fbp.ToBreakpoint(breakpoint);
            return S_OK;
        }
    }

    return S_FALSE; // Stopped at break, but breakpoint not found.
}

HRESULT FuncBreakpoints::ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &funcBreakpoints : m_funcBreakpoints)
    {
        ManagedFuncBreakpoint &fb = funcBreakpoints.second;

        if (fb.IsResolved() ||
            FAILED(ResolveFuncBreakpointInModule(pModule, fb)))
            continue;

        Breakpoint breakpoint;
        fb.ToBreakpoint(breakpoint);
        events.emplace_back(BreakpointChanged, breakpoint);
    }

    return S_OK;
}

HRESULT FuncBreakpoints::SetFuncBreakpoints(bool haveProcess, const std::vector<FuncBreakpoint> &funcBreakpoints,
                                            std::vector<Breakpoint> &breakpoints, std::function<uint32_t()> getId)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    // Remove old breakpoints
    std::unordered_set<std::string> funcBreakpointFuncs;
    for (const auto &fb : funcBreakpoints)
    {
        std::string fullFuncName("");
        if (!fb.module.empty())
        {
            fullFuncName = fb.module + "!";
        }
        fullFuncName += fb.func + fb.params;
        funcBreakpointFuncs.insert(fullFuncName);
    }
    for (auto it = m_funcBreakpoints.begin(); it != m_funcBreakpoints.end();)
    {
        if (funcBreakpointFuncs.find(it->first) == funcBreakpointFuncs.end())
            it = m_funcBreakpoints.erase(it);
        else
            ++it;
    }

    if (funcBreakpoints.empty())
        return S_OK;


    // Export function breakpoints
    // Note, VSCode and MI/GDB protocols requires, that "breakpoints" and "funcBreakpoints" must have same indexes for same breakpoints.

    for (const auto &fb : funcBreakpoints)
    {
        std::string fullFuncName("");

        if (!fb.module.empty())
            fullFuncName = fb.module + "!";

        fullFuncName += fb.func + fb.params;

        Breakpoint breakpoint;

        auto b = m_funcBreakpoints.find(fullFuncName);
        if (b == m_funcBreakpoints.end())
        {
            // New function breakpoint
            ManagedFuncBreakpoint fbp;
            fbp.id = getId();
            fbp.module = fb.module;
            fbp.name = fb.func;
            fbp.params = fb.params;
            fbp.condition = fb.condition;

            if (haveProcess)
                ResolveFuncBreakpoint(fbp);

            fbp.ToBreakpoint(breakpoint);
            m_funcBreakpoints.insert(std::make_pair(fullFuncName, std::move(fbp)));
        }
        else
        {
            ManagedFuncBreakpoint &fbp = b->second;

            fbp.condition = fb.condition;
            fbp.ToBreakpoint(breakpoint);
        }

        breakpoints.push_back(breakpoint);
    }

    return S_OK;
}

HRESULT FuncBreakpoints::UpdateBreakpointsOnHotReload(ICorDebugModule *pModule, std::unordered_set<mdMethodDef> &methodTokens, std::vector<BreakpointEvent> &events)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    HRESULT Status;

    for (auto &funcBreakpoints : m_funcBreakpoints)
    {
        ManagedFuncBreakpoint &fbp = funcBreakpoints.second;
        bool initiallyResolved = !fbp.funcBreakpoints.empty();

        ResolvedFBP fbpResolved;
        IfFailRet(m_sharedModules->ResolveFuncBreakpointInModule(
            pModule, fbp.module, fbp.module_checked, fbp.name,
            [&](ICorDebugModule *pModule, mdMethodDef &methodToken) -> HRESULT
        {
            // Note, in case Hot Reload we ignore "resolved" status + setup breakpoints for new/changed methods only.
            if (methodTokens.find(methodToken) != methodTokens.end())
                fbpResolved.emplace_back(std::make_pair(pModule, methodToken));

            return S_OK;
        }));

        if (fbpResolved.empty() || FAILED(AddFuncBreakpoint(fbp, fbpResolved)))
            continue;

        // Remove breakpoints from old versions.
        for (auto &entry : fbpResolved)
        {
            auto is_method = [&entry](ManagedFuncBreakpoint::internalFuncBreakpoint &ifb){return ifb.methodToken == entry.second;};
            auto findIter = std::find_if(fbp.funcBreakpoints.rbegin(), fbp.funcBreakpoints.rend(), is_method);

            mdMethodDef methodToken = findIter->methodToken;
            ULONG32 methodVersion = findIter->methodVersion;
            auto end_range = std::prev(fbp.funcBreakpoints.end(), fbpResolved.size()); // Skip added into list new/changed methods breakpoints.
            for (auto it = fbp.funcBreakpoints.begin(); it != end_range;)
            {
                if (it->methodToken == methodToken && it->methodVersion != methodVersion)
                    it = fbp.funcBreakpoints.erase(it);
                else 
                    ++it;
            }
        }

        if (!initiallyResolved)
        {
            Breakpoint breakpoint;
            fbp.ToBreakpoint(breakpoint);
            events.emplace_back(BreakpointChanged, breakpoint);
        }
    }

    return S_OK;
}

HRESULT FuncBreakpoints::AddFuncBreakpoint(ManagedFuncBreakpoint &fbp, ResolvedFBP &fbpResolved)
{
    HRESULT Status;

    for (auto &entry : fbpResolved)
    {
        IfFailRet(BreakpointUtils::SkipBreakpoint(entry.first, entry.second, m_justMyCode));
        if (Status == S_OK) // S_FALSE - don't skip breakpoint
            return S_OK;

        ToRelease<ICorDebugFunction> pFunc;
        IfFailRet(entry.first->GetFunctionFromToken(entry.second, &pFunc));
        ULONG32 currentVersion; // Note, new breakpoints could be setup for last code version only, since protocols (MI, VSCode, ...) provide method name (sig) only.
        IfFailRet(pFunc->GetCurrentVersionNumber(&currentVersion));

        ULONG32 ilNextOffset;
        if (FAILED(m_sharedModules->GetNextUserCodeILOffsetInMethod(entry.first, entry.second, currentVersion, 0, ilNextOffset)))
            return S_OK;

        ToRelease<ICorDebugCode> pCode;
        IfFailRet(pFunc->GetILCode(&pCode));

        ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;
        IfFailRet(pCode->CreateBreakpoint(ilNextOffset, &iCorFuncBreakpoint));
        IfFailRet(iCorFuncBreakpoint->Activate(fbp.enabled ? TRUE : FALSE));

        CORDB_ADDRESS modAddress;
        IfFailRet(entry.first->GetBaseAddress(&modAddress));

        fbp.funcBreakpoints.emplace_back(entry.second, currentVersion, iCorFuncBreakpoint.Detach());
    }

    return S_OK;
}

HRESULT FuncBreakpoints::ResolveFuncBreakpoint(ManagedFuncBreakpoint &fbp)
{
    HRESULT Status;
    ResolvedFBP fbpResolved;

    IfFailRet(m_sharedModules->ResolveFuncBreakpointInAny(
        fbp.module, fbp.module_checked, fbp.name, 
        [&](ICorDebugModule *pModule, mdMethodDef &methodToken) -> HRESULT
    {
        fbpResolved.emplace_back(std::make_pair(pModule, methodToken));
        return S_OK;
    }));

    return AddFuncBreakpoint(fbp, fbpResolved);
}

HRESULT FuncBreakpoints::ResolveFuncBreakpointInModule(ICorDebugModule *pModule, ManagedFuncBreakpoint &fbp)
{
    HRESULT Status;
    ResolvedFBP fbpResolved;

    IfFailRet(m_sharedModules->ResolveFuncBreakpointInModule(
        pModule, fbp.module, fbp.module_checked, fbp.name,
        [&](ICorDebugModule *pModule, mdMethodDef &methodToken) -> HRESULT
    {
        fbpResolved.emplace_back(std::make_pair(pModule, methodToken));
        return S_OK;
    }));

    return AddFuncBreakpoint(fbp, fbpResolved);
}

HRESULT FuncBreakpoints::AllBreakpointsActivate(bool act)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    HRESULT Status = S_OK;
    for (auto &fbp : m_funcBreakpoints)
    {
        for (auto &funcBreakpoint : fbp.second.funcBreakpoints)
        {
            if (!funcBreakpoint.iCorFuncBreakpoint)
                continue;

            HRESULT ret = funcBreakpoint.iCorFuncBreakpoint->Activate(act ? TRUE : FALSE);
            Status = FAILED(ret) ? ret : Status;
        }
        fbp.second.enabled = act;
    }

    return Status;
}

HRESULT FuncBreakpoints::BreakpointActivate(uint32_t id, bool act)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    for (auto &fbp : m_funcBreakpoints)
    {
        if (fbp.second.id != id)
            continue;

        HRESULT Status = S_OK;
        for (auto &funcBreakpoint : fbp.second.funcBreakpoints)
        {
            if (!funcBreakpoint.iCorFuncBreakpoint)
                continue;

            HRESULT ret = funcBreakpoint.iCorFuncBreakpoint->Activate(act ? TRUE : FALSE);
            Status = FAILED(ret) ? ret : Status;
        }
        fbp.second.enabled = act;
        return Status;
    }

    return E_FAIL;
}

void FuncBreakpoints::AddAllBreakpointsInfo(std::vector<IDebugger::BreakpointInfo> &list)
{
    std::lock_guard<std::mutex> lock(m_breakpointsMutex);

    list.reserve(list.size() + m_funcBreakpoints.size());

    for (auto &pair_bp : m_funcBreakpoints)
    {
        auto &bp = pair_bp.second;

        list.emplace_back(IDebugger::BreakpointInfo{ bp.id, bp.IsVerified(), bp.enabled, bp.times, bp.condition, 
                                                     bp.name, 0, 0, bp.module, bp.params });
    }
}

} // namespace netcoredbg
