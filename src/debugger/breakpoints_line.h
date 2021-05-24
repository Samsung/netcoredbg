// Copyright (c) 2021 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <mutex>
#include <memory>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include "interfaces/idebugger.h"
#include "utils/torelease.h"

namespace netcoredbg
{

class IDebugger;
class Modules;

class LineBreakpoints
{
public:

    LineBreakpoints(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules),
        m_justMyCode(true)
    {}

    void SetJustMyCode(bool enable) { m_justMyCode = enable; };
    void DeleteAll();
    HRESULT SetLineBreakpoints(ICorDebugProcess *pProcess, const std::string &filename, const std::vector<LineBreakpoint> &lineBreakpoints,
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

    struct ManagedLineBreakpoint
    {
        uint32_t id;
        std::string module;
        int linenum;
        int endLine;
        bool enabled;
        ULONG32 times;
        std::string condition;
        // In case of code line in constructor, we could resolve multiple methods for breakpoints.
        std::vector<ToRelease<ICorDebugFunctionBreakpoint> > iCorFuncBreakpoints;

        bool IsVerified() const { return !iCorFuncBreakpoints.empty(); }

        ManagedLineBreakpoint() :
            id(0), linenum(0), endLine(0), enabled(true), times(0)
        {}

        ~ManagedLineBreakpoint()
        {
            for (auto &iCorFuncBreakpoint : iCorFuncBreakpoints)
            {
                if (iCorFuncBreakpoint)
                    iCorFuncBreakpoint->Activate(FALSE);
            }
        }

        void ToBreakpoint(Breakpoint &breakpoint, const std::string &fullname);

        ManagedLineBreakpoint(ManagedLineBreakpoint &&that) = default;
        ManagedLineBreakpoint(const ManagedLineBreakpoint &that) = delete;
    };

    struct ManagedLineBreakpointMapping
    {
        LineBreakpoint breakpoint;
        uint32_t id;
        bool enabled;
        unsigned resolved_fullname_index;
        int resolved_linenum; // if int is 0 - no resolved breakpoint available in m_lineResolvedBreakpoints

        ManagedLineBreakpointMapping() : breakpoint("", 0, ""), id(0), enabled(true), resolved_fullname_index(0), resolved_linenum(0) {}
        ~ManagedLineBreakpointMapping() = default;
    };

    std::mutex m_breakpointsMutex;
    // Resolved line breakpoints:
    // Mapped in order to fast search with mapping data (see container below):
    // resolved source full path index -> resolved line number -> list of all ManagedLineBreakpoint resolved to this line.
    std::unordered_map<unsigned, std::unordered_map<int, std::list<ManagedLineBreakpoint> > > m_lineResolvedBreakpoints;
    // Mapping for input LineBreakpoint array (input from protocol) to ManagedLineBreakpoint or unresolved breakpoint.
    // Note, instead of FuncBreakpoint for resolved breakpoint we could have changed source path and/or line number.
    // In this way we could connect new input data with previous data and properly add/remove resolved and unresolved breakpoints.
    // Container have structure for fast compare current breakpoints data with new breakpoints data from protocol:
    // path to source -> list of ManagedLineBreakpointMapping that include LineBreakpoint (from protocol) and resolve related data.
    std::unordered_map<std::string, std::list<ManagedLineBreakpointMapping> > m_lineBreakpointMapping;

    HRESULT EnableOneICorBreakpointForLine(std::list<ManagedLineBreakpoint> &bList);
    HRESULT ResolveLineBreakpoint(ICorDebugModule *pModule, ManagedLineBreakpoint &bp, const std::string &bp_fullname, unsigned &bp_fullname_index);

};

} // namespace netcoredbg
