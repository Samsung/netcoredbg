using System;
using System.Collections.Generic;

namespace NetcoreDbgTest.VSCode
{
    public class Response : ProtocolMessage {
        public int request_seq;
        public bool success;
        public string command;
        public string message;
    }

    public class StackTraceResponse : Response {
        public StackTraceResponseBody body;
    }

    public class StackTraceResponseBody {
        public List<StackFrame> stackFrames;
        public int ?totalFrames;
    }

    public class StackFrame {
        public Int64 id;
        public string name;
        public Source source;
        public int line;
        public int column;
        public int ?endLine;
        public int ?endColumn;
        public dynamic moduleId = null;
        public string presentationHint; // "normal" | "label" | "subtle"
    }

    public class ThreadsResponse : Response {
        public ThreadsResponseBody body;
    }

    public class ThreadsResponseBody {
        public List<Thread> threads;
    }

    public class Thread {
        public int id;
        public string name;
    }

    public class ScopesResponse : Response {
        public ScopesResponseBody body;
    }

    public class ScopesResponseBody {
        public List<Scope> scopes;
    }

    public class Scope {
        public string name;
        public int ?variablesReference;
        public int ?namedVariables;
        public int ?indexedVariables;
        public bool ?expensive;
        public Source source;
        public int ?line;
        public int ?column;
        public int ?endLine;
        public int ?endColumn;
    }

    public class VariablesResponse : Response {
        public VariablesResponseBody body;
    }

    public class VariablesResponseBody {
        public List<Variable> variables;
    }

    public class Variable {
        public string name;
        public string value;
        public string type;
        public VariablePresentationHint presentationHint;
        public string evaluateName;
        public int variablesReference;
        public int ?namedVariables;
        public int ?indexedVariables;
    }

    public class VariablePresentationHint {
        public string kind;
        public List<string> attributes;
        public string visibility;
    }

    public class EvaluateResponse : Response {
        public EvaluateResponseBody body;
    }

    public class EvaluateResponseBody {
        public string result;
        public string type;
        public VariablePresentationHint presentationHint;
        public int variablesReference;
        public int ?namedVariables;
        public int ?indexedVariables;
    }

    public class SetVariableResponse : Response {
        public SetVariableResponseBody body;
    }

    public class SetVariableResponseBody {
        public string value;
        public string type;
        public int ?variablesReference;
        public int ?namedVariables;
        public int ?indexedVariables;
    }

    public class Breakpoint {
        public int ?id;
        public bool verified;
        public string message;
        public Source source;
        public int ?line;
        public int ?column;
        public int ?endLine;
        public int ?endColumn;
        public string instructionReference;
        public int ?offset;
    }

    public class SetBreakpointsResponseBody {
        public List<Breakpoint> breakpoints;
    }

    public class SetBreakpointsResponse : Response {
        public SetBreakpointsResponseBody body;
    }

    public class ExceptionInfoResponse : Response {
        public ExceptionInfoResponseBody body;
    }

    public class ExceptionInfoResponseBody {
        public string exceptionId;
        public string? description;
        public string breakMode; // "never" | "always" | "unhandled" | "userUnhandled"
        public ExceptionDetails? details;
    }

    public class ExceptionDetails {
        public string? message;
        public string? typeName;
        public string? fullTypeName;
        public string? evaluateName;
        public string stackTrace;
        public List<ExceptionDetails> innerException;
    }

}
