// Copyright (c) 2023 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef INTEROP_DEBUGGING

#include "debugger/interop_ptrace_helpers.h"
#include <mutex>
#include <memory>
#include <list>
#include <unordered_map>
#include "interfaces/idebugger.h"


namespace netcoredbg
{
namespace InteropDebugging
{

class InteropBreakpoints;
class InteropLibraries;

class InteropLineBreakpoints
{
public:

    InteropLineBreakpoints(std::shared_ptr<InteropBreakpoints> &sharedInteropBreakpoints) :
        m_sharedInteropBreakpoints(sharedInteropBreakpoints)
    {}

    // Remove all native breakpoints at interop detach.
    void RemoveAllAtDetach(pid_t pid);
    // Return `false` in case of error, `pid` have `0` in case no debuggee process available.
    bool SetLineBreakpoints(pid_t pid, InteropLibraries *pInteropLibraries, const std::string &filename, const std::vector<LineBreakpoint> &lineBreakpoints,
                            std::vector<Breakpoint> &breakpoints, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads, std::function<uint32_t()> getId);
    // In case of error, return `errno` code.
    int AllBreakpointsActivate(pid_t pid, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    // In case of error, return `errno` code.
    int BreakpointActivate(pid_t pid, uint32_t id, bool act, std::function<void()> StopAllThreads, std::function<void(std::uintptr_t)> FixAllThreads);
    void AddAllBreakpointsInfo(std::vector<IDebugger::BreakpointInfo> &list);

    bool IsLineBreakpoint(std::uintptr_t addr, Breakpoint &breakpoint);
    void LoadModule(pid_t pid, std::uintptr_t startAddr, InteropLibraries *pInteropLibraries, std::vector<BreakpointEvent> &events);
    // Remove all related to unloaded library breakpoints entries in data structures.
    void UnloadModule(std::uintptr_t startAddr, std::uintptr_t endAddr, std::vector<BreakpointEvent> &events);

    struct InteropLineBreakpoint
    {
        uint32_t m_id;
        std::string m_module; // dynamic library for interop
        std::string m_sourceFullPath; // resolved source full path
        int m_linenum;
        int m_endLine;
        bool m_enabled;
        bool m_isThumbCode; // Is resolved address is thumb code.
        ULONG32 m_times;
        // TODO `m_condition` support

        InteropLineBreakpoint() :
            m_id(0), m_linenum(0), m_endLine(0), m_enabled(true), m_isThumbCode(false), m_times(0)
        {}

        void ToBreakpoint(Breakpoint &breakpoint, bool verified) const;

        InteropLineBreakpoint(InteropLineBreakpoint &&that) = default;
        InteropLineBreakpoint(const InteropLineBreakpoint &that) = delete;
        InteropLineBreakpoint& operator=(InteropLineBreakpoint &&that) = default;
        InteropLineBreakpoint& operator=(const InteropLineBreakpoint &that) = delete;
    };

private:

    std::shared_ptr<InteropBreakpoints> m_sharedInteropBreakpoints;

    struct InteropLineBreakpointMapping
    {
        LineBreakpoint m_breakpoint;
        uint32_t m_id;
        bool m_enabled;
        std::uintptr_t m_resolved_brkAddr;

        InteropLineBreakpointMapping() : m_breakpoint("", 0, ""), m_id(0), m_enabled(true), m_resolved_brkAddr(0) {}
        ~InteropLineBreakpointMapping() = default;
    };

    std::mutex m_breakpointsMutex;
    // Resolved line breakpoints:
    // Mapped in order to fast search with mapping data (see container below):
    // resolved mem address -> list of all InteropLineBreakpoint resolved to this address.
    std::unordered_map<std::uintptr_t, std::list<InteropLineBreakpoint> > m_lineResolvedBreakpoints;
    // Mapping for input LineBreakpoint array (input from protocol) to InteropLineBreakpoint or unresolved breakpoint.
    // Note, instead of FuncBreakpoint for resolved breakpoint we could have changed source path and/or line number.
    // In this way we could connect new input data with previous data and properly add/remove resolved and unresolved breakpoints.
    // Container have structure for fast compare current breakpoints data with new breakpoints data from protocol:
    // path to source -> list of InteropLineBreakpointMapping that include LineBreakpoint (from protocol) and resolve related data.
    std::unordered_map<std::string, std::list<InteropLineBreakpointMapping> > m_lineBreakpointMapping;

};

} // namespace InteropDebugging
} // namespace netcoredbg

#endif // INTEROP_DEBUGGING
