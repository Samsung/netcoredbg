// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include "cor.h"
#include "cordebug.h"

#include <cstdint>
#include <string>
#include <list>
#include <functional>
#include <mutex>
#include <memory>
#include "interfaces/idebugger.h"
#include "debugger/exceptionbreakpointstorage.h"
#include "torelease.h"

namespace netcoredbg
{

class Modules;

class Breakpoints
{
    class AnyBPReference;

    struct ManagedBreakpoint
    {
        // In case of code line in constructor, we could resolve multiple methods for breakpoints.
        struct BreakpointElement
        {
            mdMethodDef methodToken;
            ULONG32 ilOffset;
            ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;

            BreakpointElement(mdMethodDef mt, ULONG32 il, ICorDebugFunctionBreakpoint *bp) :
                methodToken(mt), ilOffset(il), iCorFuncBreakpoint(bp) {}
        };

        uint32_t id;
        std::string module;
        CORDB_ADDRESS modAddress;
        std::string fullname;
        int linenum;
        int endLine;
        bool enabled;
        ULONG32 times;
        std::string condition;
        std::vector<BreakpointElement> breakpoints;

        bool IsVerified() const { return modAddress != 0; }

        ManagedBreakpoint() :
            id(0), modAddress(0), linenum(0), endLine(0), enabled(true), times(0)
        {}

        ~ManagedBreakpoint()
        {
            for (auto &bp : breakpoints)
            {
                if (bp.iCorFuncBreakpoint)
                    bp.iCorFuncBreakpoint->Activate(FALSE);
            }
        }

        void ToBreakpoint(Breakpoint &breakpoint);

        ManagedBreakpoint(ManagedBreakpoint &&that) = default;
        ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
    };

    struct ManagedFunctionBreakpoint
    {
        struct FuncBreakpointElement
        {
            CORDB_ADDRESS modAddress;
            mdMethodDef methodToken;
            ToRelease<ICorDebugFunctionBreakpoint> iCorFuncBreakpoint;

            FuncBreakpointElement(CORDB_ADDRESS ma, mdMethodDef mt, ICorDebugFunctionBreakpoint *fb) :
                modAddress(ma), methodToken(mt), iCorFuncBreakpoint(fb) {}
        };

        uint32_t id;
        std::string module;
        bool module_checked; // in case "module" provided, we need mark that module was checked or not (since function could be not found by name)
        std::string name;
        std::string params;
        ULONG32 times;
        bool enabled;
        std::string condition;
        std::vector<FuncBreakpointElement> breakpoints;

        bool IsResolved() const { return module_checked; }
        bool IsVerified() const { return !breakpoints.empty(); }

        ManagedFunctionBreakpoint() :
            id(0), module_checked(false), times(0), enabled(true)
        {}

        ~ManagedFunctionBreakpoint()
        {
            for (auto &fb : breakpoints)
            {
                if (fb.iCorFuncBreakpoint)
                    fb.iCorFuncBreakpoint->Activate(FALSE);
            }
        }

        void ToBreakpoint(Breakpoint &breakpoint) const;

        ManagedFunctionBreakpoint(ManagedFunctionBreakpoint &&that) = default;
        ManagedFunctionBreakpoint(const ManagedFunctionBreakpoint &that) = delete;
    };

    struct SourceBreakpointMapping
    {
        SourceBreakpoint breakpoint;
        uint32_t id = 0;
        bool enabled;
        std::string resolved_fullname; // if string is empty - no resolved breakpoint available in m_resolvedBreakpoints
        int resolved_linenum = 0;

        SourceBreakpointMapping() : breakpoint("", 0, ""), id(0), enabled(true), resolved_fullname(), resolved_linenum(0) {}
        ~SourceBreakpointMapping() = default;
    };

    std::shared_ptr<Modules> m_sharedModules;
    uint32_t m_nextBreakpointId;
    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, std::unordered_map<int, std::list<ManagedBreakpoint> > > m_srcResolvedBreakpoints;
    std::unordered_map<std::string, std::list<SourceBreakpointMapping> > m_srcInitialBreakpoints;

    std::unordered_map<std::string, ManagedFunctionBreakpoint > m_funcBreakpoints;
    ExceptionBreakpointStorage m_exceptionBreakpoints;

    HRESULT ResolveBreakpoint(ICorDebugModule *pModule, ManagedBreakpoint &bp);

    HRESULT ResolveFunctionBreakpointInModule(ICorDebugModule *pModule, ManagedFunctionBreakpoint &bp);
    HRESULT ResolveFunctionBreakpoint(ManagedFunctionBreakpoint &fbp);

    bool m_stopAtEntry;
    mdMethodDef m_entryPoint;
    ToRelease<ICorDebugFunctionBreakpoint> m_entryBreakpoint;

    HRESULT EnableOneICorBreakpointForLine(std::list<ManagedBreakpoint> &bList);
    HRESULT TrySetupEntryBreakpoint(ICorDebugModule *pModule);
    bool HitEntry(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);

    template <typename BreakpointType>
    HRESULT HandleEnabled(BreakpointType &bp, IDebugger *debugger, ICorDebugThread *pThread, Breakpoint &breakpoint);

    HRESULT HitManagedBreakpoint(
        IDebugger *debugger,
        ICorDebugThread *pThread,
        Breakpoint &breakpoint);

    HRESULT HitManagedFunctionBreakpoint(
        IDebugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint,
        Breakpoint &breakpoint);

public:
    Breakpoints(std::shared_ptr<Modules> &sharedModules) :
        m_sharedModules(sharedModules), m_nextBreakpointId(1), m_stopAtEntry(false), m_entryPoint(mdMethodDefNil) {}

    HRESULT HitBreakpoint(
        IDebugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint,
        Breakpoint &breakpoint,
        bool &atEntry);

    void DeleteAllBreakpoints();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

    HRESULT SetBreakpoints(
        ICorDebugProcess *pProcess,
        const std::string& filename,
        const std::vector<SourceBreakpoint> &srcBreakpoints,
        std::vector<Breakpoint> &breakpoints);

    HRESULT SetFunctionBreakpoints(
        ICorDebugProcess *pProcess,
        const std::vector<FunctionBreakpoint> &funcBreakpoints,
        std::vector<Breakpoint> &breakpoints);

    void SetStopAtEntry(bool stopAtEntry);

    HRESULT InsertExceptionBreakpoint(const ExceptionBreakMode &mode, const std::string &name, uint32_t &output);
    HRESULT DeleteExceptionBreakpoint(const uint32_t id);
    HRESULT GetExceptionBreakMode(ExceptionBreakMode &mode, const std::string &name);
    bool MatchExceptionBreakpoint(CorDebugExceptionCallbackType dwEventType, const std::string &exceptionName, const ExceptionBreakCategory category);
    // This function allows to enumerate breakpoints (sorted by number).
    // Callback which is called for each breakpoint might return `false` to stop iteration
    // over breakpoints list.
    void EnumerateBreakpoints(std::function<bool (const IDebugger::BreakpointInfo&)>&& callback);

    HRESULT BreakpointActivate(uint32_t id, bool act);
    HRESULT AllBreakpointsActivate(bool act);
};

} // namespace netcoredbg
