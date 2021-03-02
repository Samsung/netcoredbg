// Copyright (c) 2020 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#include <cstdint>
#include <string>
#include <list>
#include <functional>
#include "string_view.h"
#include "metadata/modules.h"
#include "debugger/debugger.h"
#include "protocols/protocol.h"
#include "debugger/exceptionbreakpointstorage.h"

namespace netcoredbg
{

using Utility::string_view;

class Breakpoints
{
    class AnyBPReference;

    struct ManagedBreakpoint {
        uint32_t id;
        CORDB_ADDRESS modAddress;
        mdMethodDef methodToken;
        ULONG32 ilOffset;
        std::string fullname;
        int linenum;
        int endLine;
        ToRelease<ICorDebugBreakpoint> iCorBreakpoint;
        bool enabled;
        ULONG32 times;
        std::string condition;

        bool IsResolved() const { return modAddress != 0; }

        ManagedBreakpoint();
        ~ManagedBreakpoint();

        void ToBreakpoint(Breakpoint &breakpoint);

        ManagedBreakpoint(ManagedBreakpoint &&that) = default;
        ManagedBreakpoint(const ManagedBreakpoint &that) = delete;
    };

    struct ManagedFunctionBreakpoint {

        struct FuncBreakpointElement {
            CORDB_ADDRESS modAddress;
            mdMethodDef methodToken;
            ToRelease<ICorDebugFunctionBreakpoint> funcBreakpoint;

            FuncBreakpointElement(CORDB_ADDRESS ma, mdMethodDef mt, ICorDebugFunctionBreakpoint *fb) :
                modAddress(ma), methodToken(mt), funcBreakpoint(fb) {}
        };

        uint32_t id;
        std::string module;
        std::string name;
        std::string params;
        ULONG32 times;
        bool enabled;
        std::string condition;
        std::vector<FuncBreakpointElement> breakpoints;

        bool IsResolved() const { return !breakpoints.empty(); }

        ManagedFunctionBreakpoint() : id(0),
                                      times(0),
                                      enabled(true)
        {}

        ~ManagedFunctionBreakpoint()
        {
            for (auto &fbel : breakpoints)
            {
                if (fbel.funcBreakpoint)
                    fbel.funcBreakpoint->Activate(0);
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
        std::string resolved_fullname; // if string is empty - no resolved breakpoint available in m_resolvedBreakpoints
        int resolved_linenum = 0;

        SourceBreakpointMapping() : breakpoint(0, ""), id(0), resolved_fullname(), resolved_linenum(0) {}
        ~SourceBreakpointMapping() = default;
    };

    Modules &m_modules;
    uint32_t m_nextBreakpointId;
    std::mutex m_breakpointsMutex;
    std::unordered_map<std::string, std::unordered_map<int, std::list<ManagedBreakpoint> > > m_srcResolvedBreakpoints;
    std::unordered_map<std::string, std::list<SourceBreakpointMapping> > m_srcInitialBreakpoints;

    std::unordered_map<std::string, ManagedFunctionBreakpoint > m_funcBreakpoints;
    ExceptionBreakpointStorage m_exceptionBreakpoints;

    HRESULT ResolveBreakpointInModule(ICorDebugModule *pModule, ManagedBreakpoint &bp);
    HRESULT ResolveBreakpoint(ManagedBreakpoint &bp);

    HRESULT ResolveFunctionBreakpointInModule(ICorDebugModule *pModule, ManagedFunctionBreakpoint &bp);
    HRESULT ResolveFunctionBreakpoint(ManagedFunctionBreakpoint &fbp);

    bool m_stopAtEntry;
    mdMethodDef m_entryPoint;
    ToRelease<ICorDebugFunctionBreakpoint> m_entryBreakpoint;

    void EnableOneICorBreakpointForLine(std::list<ManagedBreakpoint> &bList);
    HRESULT TrySetupEntryBreakpoint(ICorDebugModule *pModule);
    bool HitEntry(ICorDebugThread *pThread, ICorDebugBreakpoint *pBreakpoint);

    template <typename BreakpointType>
    HRESULT HandleEnabled(BreakpointType &bp, Debugger *debugger, ICorDebugThread *pThread, Breakpoint &breakpoint);

    HRESULT HitManagedBreakpoint(
        Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        mdMethodDef methodToken,
        Breakpoint &breakpoint);

    HRESULT HitManagedFunctionBreakpoint(Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugFrame *pFrame,
        ICorDebugBreakpoint *pBreakpoint,
        mdMethodDef methodToken,
        Breakpoint &breakpoint);

public:
    Breakpoints(Modules &modules) :
        m_modules(modules), m_nextBreakpointId(1), m_stopAtEntry(false), m_entryPoint(mdMethodDefNil) {}

    HRESULT HitBreakpoint(
        Debugger *debugger,
        ICorDebugThread *pThread,
        ICorDebugBreakpoint *pBreakpoint,
        Breakpoint &breakpoint,
        bool &atEntry);

    void DeleteAllBreakpoints();

    void TryResolveBreakpointsForModule(ICorDebugModule *pModule, std::vector<BreakpointEvent> &events);

    HRESULT SetBreakpoints(
        ICorDebugProcess *pProcess,
        std::string filename,
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
    void EnumerateBreakpoints(std::function<bool (const Debugger::BreakpointInfo&)>&& callback);
};

} // namespace netcoredbg
