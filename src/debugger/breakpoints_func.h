// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include "interfaces/idebugger.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class IDebugger;
class Modules;

class FuncBreakpoints
{
public:

    FuncBreakpoints(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules),
        m_justMyCode(true)
    {}

    void SetJustMyCode(bool enable) { m_justMyCode = enable; };
    void DeleteAll();
    HRESULT SetFuncBreakpoints(ICorDebugProcess *pProcess, const std::vector<FuncBreakpoint> &funcBreakpoints,
                               std::vector<Breakpoint> &breakpoints, std::function<uint32_t()> getId);
    HRESULT AllBreakpointsActivate(bool act);
    HRESULT BreakpointActivate(uint32_t id, bool act);
    void AddAllBreakpointsInfo(std::vector<IDebugger::BreakpointInfo> &list);

    // Important! Callbacks related methods must control return for succeeded return code.
    // Do not allow debugger API return succeeded (uncontrolled) return code.
    // Bad :
    //     return pProcess->Continue(0);
    // Good:
    //     IfFailRet(pProcess->Continue(0));
    //     return S_OK;
    HRESULT ManagedCallbackBreakpoint(IDebugger *debugger, ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint, Breakpoint &breakpoint);
    HRESULT ManagedCallbackLoadModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

private:

    std::shared_ptr<Modules> m_sharedModules;
    bool m_justMyCode;

    struct ManagedFuncBreakpoint
    {
        uint32_t id;
        std::string module;
        bool module_checked; // in case "module" provided, we need mark that module was checked or not (since function could be not found by name)
        std::string name;
        std::string params;
        ULONG32 times;
        bool enabled;
        std::string condition;
        std::vector<ToRelease<ICorDebugFunctionBreakpoint> > iCorFuncBreakpoints;

        bool IsResolved() const { return module_checked; }
        bool IsVerified() const { return !iCorFuncBreakpoints.empty(); }

        ManagedFuncBreakpoint() :
            id(0), module_checked(false), times(0), enabled(true)
        {}

        ~ManagedFuncBreakpoint()
        {
            for (auto &iCorFuncBreakpoint : iCorFuncBreakpoints)
            {
                if (iCorFuncBreakpoint)
                    iCorFuncBreakpoint->Activate(FALSE);
            }
        }

        void ToBreakpoint(Breakpoint &breakpoint) const;

        ManagedFuncBreakpoint(ManagedFuncBreakpoint &&that) = default;
        ManagedFuncBreakpoint(const ManagedFuncBreakpoint &that) = delete;
    };

    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, ManagedFuncBreakpoint> m_funcBreakpoints;

    typedef std::vector<std::pair<ICorDebugModule*,mdMethodDef> > ResolvedFBP;
    HRESULT AddFuncBreakpoint(ManagedFuncBreakpoint &fbp, ResolvedFBP &fbpResolved);
    HRESULT ResolveFuncBreakpointInModule(ICorDebugModule *pModule, ManagedFuncBreakpoint &fbp);
    HRESULT ResolveFuncBreakpoint(ManagedFuncBreakpoint &fbp);

};

} // namespace netcoredbg
