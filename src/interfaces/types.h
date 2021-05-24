// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#pragma once

#ifdef FEATURE_PAL
#include <pal_mstypes.h>
#else
#include <wtypes.h>
#include "palclr.h"
#endif

#include <string>
#include <tuple>
#include <vector>
#include <bitset>
#include <cassert>
#include <climits>

namespace netcoredbg
{

// This is helper class, which simplifies creation of custom scalar types
// (ones, which provide stron typing and disallow mixing with any other scalar types).
// Basically these types support equality compare operators and operator<
// (to allow using such types with STL containers).
//
template <typename T> struct CustomScalarType
{
    friend bool operator==(T a, T b) { return static_cast<typename T::ScalarType>(a) == static_cast<typename T::ScalarType>(b); }
    template <typename U> friend bool operator==(T a, U b) { return static_cast<typename T::ScalarType>(a) == b; }
    template <typename U> friend bool operator==(U a, T b) { return a  == static_cast<typename T::ScalarType>(b); }
    friend bool operator!=(T a, T b) { return !(a == b); }
    template <typename U> friend bool operator!=(T a, U b) { return !(a == b); }
    template <typename U> friend bool operator!=(U a, T b) { return !(a == b); }

    bool operator<(const T& other) const
    {
        return static_cast<typename T::ScalarType>(static_cast<const T&>(*this)) < static_cast<typename T::ScalarType>(static_cast<const T&>(other));
    }
};

// Process identifier.
class PID : public CustomScalarType<PID>
{	
public:
    typedef DWORD ScalarType;

    explicit PID(ScalarType n) : m_pid{int(n)} {}
    explicit operator ScalarType() const { return m_pid; }

private:
    int m_pid;
};

// Data type dedicated to carry thread id.
class ThreadId : public CustomScalarType<ThreadId>
{
    enum SpecialValues
    {
        InvalidValue = 0,
        AllThreadsValue = -1
    };

    int m_id;

    ThreadId(SpecialValues val) : m_id(val) {}

public:
    typedef int ScalarType;

    // This is for cases, when ThreadId isn't initialized/unknown.
    static const ThreadId Invalid;

    // This should be used as Any/All threads sign for protocol (for Emit...Event).
    static const ThreadId AllThreads;

    ThreadId() : ThreadId(Invalid) {}

    explicit ThreadId(int threadId) : m_id(threadId)
    {
        assert(threadId != InvalidValue && threadId != AllThreadsValue);
    }

    explicit ThreadId(DWORD threadId) : ThreadId(int(threadId)) {}

    explicit operator bool() const { return m_id != InvalidValue; }

    explicit operator ScalarType() const { return assert(*this), m_id; }
};

// Data type dedicated to carry stack frame depth (level).
class FrameLevel : public CustomScalarType<FrameLevel>
{
public:
    typedef int ScalarType;

    static const int MaxFrameLevel = SHRT_MAX;

    FrameLevel() : m_level(-1) {}

    explicit FrameLevel(unsigned n) : m_level{(assert(int(n) <= MaxFrameLevel), int(n))} {}
    explicit FrameLevel(int n) : FrameLevel(unsigned(n)) {}

    explicit operator ScalarType() const { return assert(*this), m_level; }
    explicit operator bool() const { return m_level != -1; }

private:
    int m_level;
};

// Unique stack frame identifier, which persist until program isn't continued.
class FrameId : public CustomScalarType<FrameId>
{
public:
    typedef int ScalarType;

    const static int32_t MaxFrameId = INT32_MAX;

    FrameId() : m_id(-1) {}
    FrameId(ThreadId, FrameLevel);
    FrameId(int); 

    explicit operator ScalarType() const noexcept { return assert(*this), m_id; }
    explicit operator bool() const { return m_id != -1; }

    ThreadId getThread() const noexcept;
    FrameLevel getLevel() const noexcept;

    static void invalidate();

private:
    ScalarType m_id;
};


// From https://github.com/Microsoft/vscode-debugadapter-node/blob/master/protocol/src/debugProtocol.ts

struct Thread
{
    ThreadId id;
    std::string name;
    bool running;

    Thread(ThreadId id, const std::string& name, bool running) : id(id), name(name), running(running) {}
};

struct Source
{
    std::string name;
    std::string path;

    Source(const std::string &path = {});
    bool IsNull() const { return name.empty() && path.empty(); }
};

struct ClrAddr
{
    uint32_t ilOffset;
    uint32_t nativeOffset;
    uint32_t methodToken;

    ClrAddr() : ilOffset(0), nativeOffset(0), methodToken(0) {}
    bool IsNull() const { return methodToken == 0; }
};

struct StackFrame
{
private:
    template <typename T> using Optional = std::tuple<T, bool>;

    mutable Optional<ThreadId> thread;
    mutable Optional<FrameLevel> level;

public:
    FrameId id;         // should be assigned only once, befor calls to GetLevel or GetThreadId.
    std::string name;
    Source source;
    int line;
    int column;
    int endLine;
    int endColumn;
    std::string moduleId;

    ClrAddr clrAddr; // exposed for MI protocol
    uint64_t addr; // exposed for MI protocol

    StackFrame() :
        thread(ThreadId{}, true), level(FrameLevel{}, true), id(),
        line(0), column(0), endLine(0), endColumn(0), addr(0) {}

    StackFrame(ThreadId threadId, FrameLevel level, const std::string& name) :
        thread(threadId, true), level(level, true), id(FrameId(threadId, level)),
        name(name), line(0), column(0), endLine(0), endColumn(0), addr(0)
    {}

    StackFrame(FrameId id) :
        thread(ThreadId{}, false), level(FrameLevel{}, false), id(id),
        line(0), column(0), endLine(0), endColumn(0), addr(0)
    {}

    FrameLevel GetLevel() const
    {
        return std::get<1>(level) ? std::get<0>(level) : std::get<0>(level) = id.getLevel();
    }

    ThreadId GetThreadId() const
    {
        return std::get<1>(thread) ? std::get<0>(thread) : std::get<0>(thread) = id.getThread();
    }
};

struct Breakpoint
{
    uint32_t id;
    bool verified;
    std::string message;
    Source source;
    int line;
    int endLine;

    uint32_t hitCount; // exposed for MI protocol
    std::string condition;
    std::string module;
    std::string funcname;
    std::string params;

    Breakpoint() : id(0), verified(false), line(0), endLine(0), hitCount(0) {}
};

enum SymbolStatus
{
    SymbolsSkipped, // "Skipped loading symbols."
    SymbolsLoaded,  // "Symbols loaded."
    SymbolsNotFound
};

struct Module
{
    std::string id;
    std::string name;
    std::string path;
    // bool isOptimized; // TODO: support both fields for VSCode protocol
    // bool isUserCode;
    SymbolStatus symbolStatus;
    uint64_t baseAddress; // exposed for MI protocol
    uint32_t size; // exposed for MI protocol

    Module() : symbolStatus(SymbolsSkipped), baseAddress(0), size(0) {}
};

enum BreakpointReason
{
    BreakpointChanged,
    BreakpointNew,
    BreakpointRemoved
};

enum StopReason
{
    StopStep,
    StopBreakpoint,
    StopException,
    StopPause,
    StopEntry
};

struct StoppedEvent
{
    StopReason reason;
    std::string description;
    ThreadId threadId;
    std::string text;
    bool allThreadsStopped;

    StackFrame frame; // exposed for MI protocol
    Breakpoint breakpoint; // exposed for MI protocol

    StoppedEvent(StopReason reason, ThreadId threadId = ThreadId::Invalid)
        :reason(reason), threadId(threadId), allThreadsStopped(true)
    {}
};

struct BreakpointEvent
{
    BreakpointReason reason;
    Breakpoint breakpoint;

    BreakpointEvent(const BreakpointReason &reason, const Breakpoint &breakpoint) : reason(reason), breakpoint(breakpoint) {}
};

struct ExitedEvent
{
    int exitCode;

    ExitedEvent(int exitCode) : exitCode(exitCode) {}
};

enum ThreadReason
{
    ThreadStarted,
    ThreadExited
};

struct ThreadEvent
{
    ThreadReason reason;
    ThreadId threadId;

    ThreadEvent(ThreadReason reason, ThreadId threadId) : reason(reason), threadId(threadId) {}
};

enum OutputCategory
{
    OutputConsole,
    OutputStdOut,
    OutputStdErr
};

enum ModuleReason
{
    ModuleNew,
    ModuleChanged,
    ModuleRemoved
};

struct ModuleEvent
{
    ModuleReason reason;
    Module module;
    ModuleEvent(ModuleReason reason, const Module &module) : reason(reason), module(module) {}
};

struct Scope
{
    std::string name;
    uint32_t variablesReference;
    int namedVariables;
    int indexedVariables;
    bool expensive;

    Scope() : variablesReference(0), namedVariables(0), expensive(false) {}

    Scope(uint32_t variablesReference, const std::string &name, int namedVariables) :
        name(name),
        variablesReference(variablesReference),
        namedVariables(namedVariables),
        indexedVariables(0),
        expensive(false)
    {}
};

// TODO: Replace strings with enums
struct VariablePresentationHint
{
    std::string kind;
    std::vector<std::string> attributes;
    std::string visibility;
};

// https://docs.microsoft.com/en-us/visualstudio/extensibility/debugger/reference/evalflags
enum enum_EVALFLAGS {
    EVAL_RETURNVALUE = 0x0002,
    EVAL_NOSIDEEFFECTS = 0x0004,
    EVAL_ALLOWBPS = 0x0008,
    EVAL_ALLOWERRORREPORT = 0x0010,
    EVAL_FUNCTION_AS_ADDRESS = 0x0040,
    EVAL_NOFUNCEVAL = 0x0080,
    EVAL_NOEVENTS = 0x1000
};

#define defaultEvalFlags 0

struct Variable
{
    std::string name;
    std::string value;
    std::string type;
    std::string module; // used by exception routine only
    VariablePresentationHint presentationHint;
    std::string evaluateName;
    uint32_t variablesReference;
    int namedVariables;
    int indexedVariables;
    int evalFlags;

    Variable(int flags = defaultEvalFlags) : variablesReference(0), namedVariables(0), indexedVariables(0), evalFlags(flags) {}
};

enum VariablesFilter
{
    VariablesNamed,
    VariablesIndexed,
    VariablesBoth
};

struct LineBreakpoint
{
    std::string module;
    int line;
    std::string condition;

    LineBreakpoint(const std::string &module,
                   int linenum,
                   const std::string &cond = std::string()) :
        module(module),
        line(linenum),
        condition(cond)
    {}
};

struct FuncBreakpoint
{
    std::string module;
    std::string func;
    std::string params;
    std::string condition;

    FuncBreakpoint(const std::string &module,
                   const std::string &func,
                   const std::string &params,
                   const std::string &cond = std::string()) :
        module(module),
        func(func),
        params(params),
        condition(cond)
    {}
};

enum struct ExceptionBreakCategory : int
{
    CLR = 0,
    MDA = 1,

    ANY, // CLR or MDA does not matter
};

class ExceptionBreakMode
{
private:
    enum Flag : int
    {
        F_Unhandled = 0,     // Always enabled for catch of System Exceptions.
        F_Throw = 1,         // All events raised by 'throw new' operator.
        F_UserUnhandled = 2, // Break on unhandled exception in user code.

        COUNT = 3            // Flag enum element counter
    };

public:
    std::bitset<Flag::COUNT> flags;
    ExceptionBreakCategory category;

private:
    void resetUnhandled() {
        flags.reset(Flag::F_Unhandled);
    }
    void setUnhandled() {
        flags.set(Flag::F_Unhandled);
    }

public:

    bool Unhandled() const {
        return flags.test(Flag::F_Unhandled);
    }

    bool Throw() const {
        return flags.test(Flag::F_Throw);
    }

    bool UserUnhandled() const {
        return flags.test(Flag::F_UserUnhandled);
    }

    void setThrow() {
        flags.set(Flag::F_Throw);
    }

    void setUserUnhandled() {
        flags.set(Flag::F_UserUnhandled);
    }

    void resetThrow() {
        flags.reset(Flag::F_Throw);
    }

    void resetUserUnhandled() {
        flags.reset(Flag::F_UserUnhandled);
    }

    // 'All', 'Never' values for VScode
    void setAll() {
        // setUnhandled() not supported
        // its looks as a problem with unconsistent state
        //setUnhandled();
        setThrow();
        setUserUnhandled();
    }

    void resetAll() {
        // resetUnhandled() not supported
        // its looks as a problem with unconsistent state
        //resetUnhandled();
        resetThrow();
        resetUserUnhandled();
    }

    bool All() const {
        return Unhandled() && Throw() && UserUnhandled();
    }

    bool Never() const {
        // Always false because Unhandled() always throw
        return !Unhandled() && !Throw() && !UserUnhandled();
    }

    // Logical extentions for freindly using of class
    bool AnyUser() const {
        return Throw() || UserUnhandled();
    }

    bool OnlyUnhandled() const {
        return Unhandled() && !Throw() && !UserUnhandled();
    }

    ExceptionBreakMode() : category(ExceptionBreakCategory::CLR) {
        flags.set(Flag::F_Unhandled);
    }
};

struct ExceptionDetails
{
    // Message contained in the exception.
    std::string message;
    // Short type name of the exception object.
    std::string typeName;
    // Fully-qualified type name of the exception object.
    std::string fullTypeName;
    // Optional expression that can be evaluated in the current scope
    // to obtain the exception object.
    std::string evaluateName;
    // Stack trace at the time the exception was thrown.
    std::string stackTrace;
    // Details of the exception contained by this exception, if any.
    std::vector<ExceptionDetails> innerException;
};

struct ExceptionInfoResponse
{
    // ID of the exception that was thrown.
    std::string exceptionId;
    // Descriptive text for the exception provided by the debug adapter.
    std::string description;
    // Mode that caused the exception notification to be raised.
    ExceptionBreakMode breakMode;
    // Detailed information about the exception.
    ExceptionDetails details;

    std::string getVSCodeBreakMode() const;
};

} // namespace netcoredbg
