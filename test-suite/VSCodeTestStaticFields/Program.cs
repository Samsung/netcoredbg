using System;
using System.IO;
using System.Collections.Generic;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void PrepareStart(string caller_trace)
        {
            InitializeRequest initializeRequest = new InitializeRequest();
            initializeRequest.arguments.clientID = "vscode";
            initializeRequest.arguments.clientName = "Visual Studio Code";
            initializeRequest.arguments.adapterID = "coreclr";
            initializeRequest.arguments.pathFormat = "path";
            initializeRequest.arguments.linesStartAt1 = true;
            initializeRequest.arguments.columnsStartAt1 = true;
            initializeRequest.arguments.supportsVariableType = true;
            initializeRequest.arguments.supportsVariablePaging = true;
            initializeRequest.arguments.supportsRunInTerminalRequest = true;
            initializeRequest.arguments.locale = "en-us";
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = ControlInfo.TargetAssemblyPath;
            launchRequest.arguments.cwd = "";
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void PrepareEnd(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasEntryPointHit(string caller_trace)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
        {
            bool wasExited = false;
            int ?exitCode = null;
            bool wasTerminated = false;

            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "exited")) {
                    wasExited = true;
                    ExitedEvent exitedEvent = JsonConvert.DeserializeObject<ExitedEvent>(resJSON);
                    exitCode = exitedEvent.body.exitCode;
                }
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated")) {
                    wasTerminated = true;
                }
                if (wasExited && exitCode == 0 && wasTerminated)
                    return true;

                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddBreakpoint(string caller_trace, string bpName, string Condition = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition));
            BreakpointLines.Add(lbp.NumLine);
        }

        public void SetBreakpoints(string caller_trace)
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = BreakpointSourceName;
            // NOTE this code works only with one source file
            setBreakpointsRequest.arguments.source.path = ControlInfo.SourceFilesPath;
            setBreakpointsRequest.arguments.lines.AddRange(BreakpointLines);
            setBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            setBreakpointsRequest.arguments.sourceModified = false;
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpName)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
                return;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Int64 DetectFrameId(string caller_trace, string bpName)
        {
            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
                return stackTraceResponse.body.stackFrames[0].id;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public int GetVariablesReference(string caller_trace, Int64 frameId, string ScopeName)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    return Scope.variablesReference == null ? 0 : (int)Scope.variablesReference;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public int GetChildVariablesReference(string caller_trace, int VariablesReference, string VariableName)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = VariablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == VariableName)
                    return Variable.variablesReference;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public int GetStaticMembersReference(string caller_trace, Int64 frameId, string VariableName)
        {
            int localsReference = GetVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, "Locals");
            int variableReference = GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, localsReference, VariableName);
            return GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, variableReference, "Static members");
        }

        public int GetElementStaticMembersReference(string caller_trace, Int64 frameId, string VariableName, string ElementName)
        {
            int localsReference = GetVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, "Locals");
            int variableReference = GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, localsReference, VariableName);
            int elementReference = GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, variableReference, ElementName);
            return GetChildVariablesReference(@"__FILE__:__LINE__"+"\n"+caller_trace, elementReference, "Static members");
        }

        // Change string and int static fields (default value) from static members of variable, check value in variables and by
        // expression.
        public void SetStaticMembers(string caller_trace, Int64 frameId, int staticsReference, string TypeName, string IntName, string StringName)
        {
            EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, "string", StringName, "null");
            SetVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, StringName, "\"x\"");
            EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, "string", StringName, "\"x\"");
            GetAndCheckValue(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, TypeName + "." + StringName, "string", "\"x\"");
            EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, "int", IntName, "0");
            SetVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, IntName, "7");
            EvalVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, staticsReference, "int", IntName, "7");
            GetAndCheckValue(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, TypeName + "." + IntName, "int", "7");
        }

        public void GetAndCheckValue(string caller_trace, Int64 frameId, string Expression, string ExpectedType, string ExpectedResult)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.Equal(ExpectedType, evaluateResponse.body.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, evaluateResponse.body.result, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        // Expected is "<type> <value>" or error message (starts with "error:").
        public void CheckResult(string caller_trace, Int64 frameId, string Expression, string Expected)
        {
            if (Expected.StartsWith("error:"))
            {
                CheckErrorAtRequest(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, Expression, Expected);
                return;
            }
            int separator = Expected.LastIndexOf(' ');
            GetAndCheckValue(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, Expression, Expected.Substring(0, separator), Expected.Substring(separator + 1));
        }

        public void SetExpressionResult(string caller_trace, Int64 frameId, string Expression, string Value, bool Success, string errMsg)
        {
            if (Success)
                SetExpression(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, Expression, Value);
            else
                ErrorSetExpression(@"__FILE__:__LINE__"+"\n"+caller_trace, frameId, Expression, Value, errMsg);
        }

        public void SetVariableResult(string caller_trace, int variablesReference, string Name, string Value, bool Success, string errMsg)
        {
            if (Success)
                SetVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, variablesReference, Name, Value);
            else
                ErrorSetVariable(@"__FILE__:__LINE__"+"\n"+caller_trace, variablesReference, Name, Value, errMsg);
        }

        public string GetVariable(string caller_trace, Int64 frameId, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            return evaluateResponse.body.result;
        }

        public void CheckErrorAtRequest(string caller_trace, Int64 frameId, string Expression, string errMsgStart)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.True(evaluateResponse.message.StartsWith(errMsgStart), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void EvalVariable(string caller_trace, int variablesReference, string Type, string Name, string Value)
        {
            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            VariablesResponse variablesResponse =
                JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);

            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == Name) {
                    if (Type != null)
                        Assert.Equal(Type, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(Value, Variable.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void SetVariable(string caller_trace, int variablesReference, string Name, string Value)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            var ret = VSCodeDebugger.Request(setVariableRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(Value, JsonConvert.DeserializeObject<SetVariableResponse>(ret.ResponseStr).body.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void ErrorSetVariable(string caller_trace, int variablesReference, string Name, string Value, string errMsg)
        {
            SetVariableRequest setVariableRequest = new SetVariableRequest();
            setVariableRequest.arguments.variablesReference = variablesReference;
            setVariableRequest.arguments.name = Name;
            setVariableRequest.arguments.value = Value;
            var ret = VSCodeDebugger.Request(setVariableRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(errMsg, JsonConvert.DeserializeObject<SetVariableResponse>(ret.ResponseStr).message, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void SetExpression(string caller_trace, Int64 frameId, string Expression, string Value)
        {
            SetExpressionRequest setExpressionRequest = new SetExpressionRequest();
            setExpressionRequest.arguments.expression = Expression;
            setExpressionRequest.arguments.value = Value;
            setExpressionRequest.arguments.frameId = (int)frameId;
            var ret = VSCodeDebugger.Request(setExpressionRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(Value, JsonConvert.DeserializeObject<SetExpressionResponse>(ret.ResponseStr).body.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void ErrorSetExpression(string caller_trace, Int64 frameId, string Expression, string Value, string errMsg)
        {
            SetExpressionRequest setExpressionRequest = new SetExpressionRequest();
            setExpressionRequest.arguments.expression = Expression;
            setExpressionRequest.arguments.value = Value;
            setExpressionRequest.arguments.frameId = (int)frameId;
            var ret = VSCodeDebugger.Request(setExpressionRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(errMsg, JsonConvert.DeserializeObject<Response>(ret.ResponseStr).message, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        // NOTE this code works only with one source file
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestStaticFields
{
    // Note, types below don't have static constructor (no static field initializers and no explicit static constructor),
    // so, debugger don't need evaluation for their static fields access.

    public class RefClass
    {
        public int field_i = 1;
    }

    public struct ValStruct
    {
        public int field_i;
    }

    public interface IRef
    {
    }

    public class RefImpl : IRef
    {
    }

    public class Outer
    {
        public class Inner
        {
        }
    }

    // Same simple name as NetcoreDbgTest.ControlInfo from loaded NetcoreDbgTest assembly.
    public class ControlInfo
    {
    }

    public enum TestEnum
    {
        One,
        Two
    }

    public class NoCctor
    {
        public static int s_int;
        public static string s_string;
        public static RefClass s_ref;
        public static ValStruct s_struct;
        public static IRef s_iface;
        public static Outer.Inner s_inner;
        public static ControlInfo s_collide;

        [ThreadStatic] public static int t_int;
        [ThreadStatic] public static long t_long;
        [ThreadStatic] public static bool t_bool;
        [ThreadStatic] public static double t_double;
        [ThreadStatic] public static string t_string;
        [ThreadStatic] public static object t_object;
        [ThreadStatic] public static RefClass t_ref;
        [ThreadStatic] public static volatile RefClass t_volatile_ref;
        [ThreadStatic] public static IRef t_iface;
        [ThreadStatic] public static Outer.Inner t_inner;
        [ThreadStatic] public static ControlInfo t_collide;
        [ThreadStatic] public static System.Text.StringBuilder t_other_module_ref;
        [ThreadStatic] public static List<int> t_list;
        [ThreadStatic] public static ValStruct t_struct;
        [ThreadStatic] public static int[] t_array;

        public int field_i = 2;
    }

    // Thread static fields with storage, that allocated at first access by thread.
    public class NoCctorThreadStorage
    {
        [ThreadStatic] public static long t_long0;
        [ThreadStatic] public static long t_long1;
        [ThreadStatic] public static long t_long2;
        [ThreadStatic] public static long t_long3;
        [ThreadStatic] public static long t_long4;
        [ThreadStatic] public static long t_long5;
        [ThreadStatic] public static long t_long6;
        [ThreadStatic] public static long t_long7;
    }

    // Used by debugger only at worker thread breakpoint.
    public class NoCctorWorker
    {
        public static int s_int;
        [ThreadStatic] public static long t_long0;
        [ThreadStatic] public static long t_long1;
        [ThreadStatic] public static long t_long2;
        [ThreadStatic] public static long t_long3;
        [ThreadStatic] public static long t_long4;
        [ThreadStatic] public static long t_long5;
        [ThreadStatic] public static long t_long6;
        [ThreadStatic] public static long t_long7;
        [ThreadStatic] public static string t_string;
    }

    // Used by debugger only at second worker thread breakpoint.
    public class NoCctorWorker2
    {
        [ThreadStatic] public static string t_string;
        [ThreadStatic] public static RefClass t_ref;
        [ThreadStatic] public static long t_long0;
        [ThreadStatic] public static long t_long1;
        [ThreadStatic] public static long t_long2;
        [ThreadStatic] public static long t_long3;
        [ThreadStatic] public static long t_long4;
        [ThreadStatic] public static long t_long5;
        [ThreadStatic] public static long t_long6;
        [ThreadStatic] public static long t_long7;
    }

    public struct NoCctorStruct
    {
        public static int s_int;
        [ThreadStatic] public static int t_int;

        public int field_i;
    }

    // Not used by program code at all.
    public class NoCctorUntouched
    {
        public static int s_int;
        public static RefClass s_ref;
        [ThreadStatic] public static int t_int;
    }

    // Classes below are not used by program code at all.
    public class NeverInstantiated
    {
    }

    public class NoCctorColdStruct
    {
        public static ValStruct s_struct;
    }

    public class NoCctorColdArray
    {
        public static int[] s_array;
    }

    public class NoCctorColdIntPtr
    {
        public static IntPtr s_intptr;
    }

    public class NoCctorColdUIntPtr
    {
        public static UIntPtr s_uintptr;
    }

    public class NoCctorColdEnum
    {
        public static TestEnum s_enum;
    }

    public class NoCctorColdStructAssign
    {
        public static ValStruct s_struct;
    }

    public class NoCctorColdClass
    {
        public static NeverInstantiated s_ref;
    }

    public class NoCctorColdIntAssign
    {
        public static int s_int;
    }

    public class NoCctorColdStringAssign
    {
        public static string s_string;
    }

    public class NoCctorColdThreadStatic
    {
        [ThreadStatic] public static System.Text.StringBuilder t_other_module_ref;
    }

    // Static fields not used by program code.
    public class NoCctorInstanceOnly
    {
        public static int s_int;
        public static string s_string;

        public int field_i = 3;
    }

    public struct NoCctorLocalStruct
    {
        public static int s_int;
        public static string s_string;

        public int field_i;
    }

    public struct NoCctorArrayStruct
    {
        public static int s_int;
        public static string s_string;

        public int field_i;
    }

    public struct NoCctorBoxedStruct
    {
        public static int s_int;
        public static string s_string;

        public int field_i;
    }

    public class NoCctorUntouchedBase
    {
        public static int base_s_int;
        public static string base_s_string;
    }

    public class DerivedFromUntouchedBase : NoCctorUntouchedBase
    {
        public int field_i = 4;
    }

    public class DerivedFromOtherModuleBase : VSCodeTestStaticFieldsLib.NoCctorOtherModuleBase
    {
        public int field_i = 5;
    }

    public class NoCctorConsts
    {
        public const int c_int = 3;
        public const string c_string = "const";
        public const TestEnum c_enum = TestEnum.Two;
    }

    public class NoCctorBase
    {
        public static int base_s_int;
    }

    public class DerivedFromNoCctor : NoCctorBase
    {
        public static int derived_s_int;
    }

    public class NoCctorMembers
    {
        public static int s_int;
        public static int s_prop { get { return s_int + 1; } }
        public static int GetInt() { return s_int + 2; }
    }

    public class NoCctorMethodHolder
    {
        public static int s_int;
        public static RefClass s_ref;
        [ThreadStatic] public static string t_string;

        public static void StaticMethod()
        {
            long allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy1 = 1;                                                 Label.Breakpoint("bp_static_method");
            long allocStaticMethod = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_static_method", "bp_static_method_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_static_method");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_static_method");

                // Static members of method's class (first access by debugger).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "s_int", "int", "81");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "t_string", "string", "\"holder\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "s_ref.field_i", "int", "1");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                                 Label.Breakpoint("bp_static_method_alloc");

            Label.Checkpoint("bp_static_method_alloc", "bp_worker", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_static_method_alloc");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_static_method_alloc");

                // No type object creation (no allocation by this thread) during "bp_static_method" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "allocStaticMethod", "long", "0");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }

    // Generic type without static constructor, debugger use type object for generic types.
    public class NoCctorGeneric<T>
    {
        public static T s_value;
        public static int s_int;
        [ThreadStatic] public static T t_value;
        [ThreadStatic] public static int t_int;

        public T field_value;
    }

    public class Counters
    {
        public static int cctor_runs;
        public static int generic_cctor_runs;
        public static int base_cctor_runs;
        public static int base2_cctor_runs;
    }

    // Types below have static constructor, debugger should run it before static fields access (type object creation).

    public class WithCctor
    {
        public static int s_int = 5;

        static WithCctor()
        {
            Counters.cctor_runs++;
        }
    }

    public class WithCctorThreadStorage
    {
        public static int s_int = 8;
        [ThreadStatic] public static long t_long0;
        [ThreadStatic] public static long t_long1;
        [ThreadStatic] public static long t_long2;
        [ThreadStatic] public static long t_long3;
        [ThreadStatic] public static long t_long4;
        [ThreadStatic] public static long t_long5;
        [ThreadStatic] public static long t_long6;
        [ThreadStatic] public static long t_long7;
    }

    public class WithCctorGeneric<T>
    {
        public static int s_int = 6;

        static WithCctorGeneric()
        {
            Counters.generic_cctor_runs++;
        }
    }

    public class BaseWithCctor
    {
        public static int base_s_int = 7;

        static BaseWithCctor()
        {
            Counters.base_cctor_runs++;
        }
    }

    public class DerivedNoCctor : BaseWithCctor
    {
        public static int derived_s_int;
    }

    // Not used by program code at all, base static constructor not run before debugger access.
    public class BaseWithCctor2
    {
        public static int base2_s_int = 9;

        static BaseWithCctor2()
        {
            Counters.base2_cctor_runs++;
        }
    }

    public class DerivedNoCctor2 : BaseWithCctor2
    {
        public static int derived2_s_int;
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_main", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_main");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_main_alloc");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_cold_class");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_cold_class_alloc");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_static_method");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_static_method_alloc");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_worker");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_worker_alloc");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_worker2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_worker2_alloc");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp_cctor");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            NoCctor.s_int = 11;
            NoCctor.s_string = "s_string";
            NoCctor.s_ref = new RefClass();
            NoCctor.s_struct.field_i = 12;
            NoCctor.t_int = 21;
            NoCctor.t_long = 22;
            NoCctor.t_bool = true;
            NoCctor.t_double = 2.5;
            NoCctor.t_string = "t_string";
            NoCctor.t_object = new RefClass();
            NoCctor.t_ref = new RefClass();
            NoCctor.t_volatile_ref = new RefClass();
            NoCctor.t_iface = new RefImpl();
            NoCctor.t_inner = new Outer.Inner();
            NoCctor.t_collide = new ControlInfo();
            NoCctor.t_other_module_ref = new System.Text.StringBuilder();
            NoCctor.t_list = new List<int>();
            NoCctor.t_struct.field_i = 23;
            NoCctor.t_array = new int[] { 1, 2 };
            NoCctorStruct.s_int = 31;
            NoCctorStruct.t_int = 32;
            NoCctorThreadStorage.t_long0 = 60;
            NoCctorThreadStorage.t_long7 = 67;
            NoCctorWorker.s_int = 90;
            NoCctorWorker.t_long0 = 80;
            NoCctorWorker.t_string = "main";
            NoCctorMethodHolder.s_int = 81;
            NoCctorMethodHolder.s_ref = new RefClass();
            NoCctorMethodHolder.t_string = "holder";
            NoCctorMembers.s_int = 50;
            DerivedNoCctor.derived_s_int = 51;
            NoCctorBase.base_s_int = 70;
            DerivedFromNoCctor.derived_s_int = 71;
            NoCctorGeneric<int>.s_value = 41;
            NoCctorGeneric<int>.s_int = 42;
            NoCctorGeneric<int>.t_value = 43;
            NoCctorGeneric<int>.t_int = 44;
            NoCctorGeneric<string>.s_value = "generic_s_value";
            NoCctorGeneric<string>.t_value = "generic_t_value";

            NoCctor noCctor = new NoCctor();
            NoCctorThreadStorage noCctorThreadStorage = new NoCctorThreadStorage();
            NoCctorStruct noCctorStruct = new NoCctorStruct();
            NoCctorGeneric<int> noCctorGeneric = new NoCctorGeneric<int>();
            DerivedNoCctor derivedNoCctor = new DerivedNoCctor();
            DerivedFromNoCctor derivedFromNoCctor = new DerivedFromNoCctor();
            NoCctorInstanceOnly noCctorInstanceOnly = new NoCctorInstanceOnly();
            NoCctorLocalStruct noCctorLocalStruct = new NoCctorLocalStruct();
            NoCctorArrayStruct[] noCctorArrayStruct = new NoCctorArrayStruct[1];
            object noCctorBoxedStruct = new NoCctorBoxedStruct();
            DerivedFromUntouchedBase derivedFromUntouchedBase = new DerivedFromUntouchedBase();
            DerivedFromOtherModuleBase derivedFromOtherModuleBase = new DerivedFromOtherModuleBase();

            long allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy1 = 1;                                                 Label.Breakpoint("bp_main");
            long allocMain = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_main", "bp_main_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_main");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_main");

                // Watch (evaluate request), static and thread static fields with value.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_int", "int", "11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_string", "string", "\"s_string\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_ref", "VSCodeTestStaticFields.RefClass", "{VSCodeTestStaticFields.RefClass}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_ref.field_i", "int", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_struct", "VSCodeTestStaticFields.ValStruct", "{VSCodeTestStaticFields.ValStruct}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_struct.field_i", "int", "12");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_iface", "VSCodeTestStaticFields.IRef", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_inner", "VSCodeTestStaticFields.Outer.Inner", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_collide", "VSCodeTestStaticFields.ControlInfo", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_int", "int", "21");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_long", "long", "22");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_bool", "bool", "true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_double", "double", "2.5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_string", "string", "\"t_string\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_object", "VSCodeTestStaticFields.RefClass", "{VSCodeTestStaticFields.RefClass}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_ref", "VSCodeTestStaticFields.RefClass", "{VSCodeTestStaticFields.RefClass}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_volatile_ref", "VSCodeTestStaticFields.RefClass", "{VSCodeTestStaticFields.RefClass}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_iface", "VSCodeTestStaticFields.RefImpl", "{VSCodeTestStaticFields.RefImpl}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_inner", "VSCodeTestStaticFields.Outer.Inner", "{VSCodeTestStaticFields.Outer.Inner}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_collide", "VSCodeTestStaticFields.ControlInfo", "{VSCodeTestStaticFields.ControlInfo}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_list", "System.Collections.Generic.List<int>", "{System.Collections.Generic.List<int>}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_struct.field_i", "int", "23");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_array", "int[]", "{int[2]}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorStruct.s_int", "int", "31");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorStruct.t_int", "int", "32");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long0", "long", "60");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long7", "long", "67");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "VSCodeTestStaticFields.NoCctor.s_int", "int", "11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "VSCodeTestStaticFields.NoCctor.t_string", "string", "\"t_string\"");

                // Type that was never used by program code.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorUntouched.s_int", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorUntouched.s_ref", "VSCodeTestStaticFields.RefClass", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorUntouched.t_int", "int", "0");

                // Variables (static members of variable).
                int staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctor");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "s_int", "11");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "string", "s_string", "\"s_string\"");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "VSCodeTestStaticFields.RefClass", "s_ref", "{VSCodeTestStaticFields.RefClass}");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "VSCodeTestStaticFields.ValStruct", "s_struct", "{VSCodeTestStaticFields.ValStruct}");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "t_int", "21");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "long", "t_long", "22");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "bool", "t_bool", "true");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "double", "t_double", "2.5");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "string", "t_string", "\"t_string\"");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int[]", "t_array", "{int[2]}");
                staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorStruct");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "s_int", "31");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "t_int", "32");

                // Base type without static constructor.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "DerivedFromNoCctor.derived_s_int", "int", "71");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "DerivedFromNoCctor.base_s_int", "int", "70");
                staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "derivedFromNoCctor");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "derived_s_int", "71");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "base_s_int", "70");

                // Change static and thread static fields (with storage).
                staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctor");
                Context.SetVariable(@"__FILE__:__LINE__", staticsReference, "s_int", "111");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "s_int", "111");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "NoCctor.t_int", "121");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_int", "int", "121");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                                 Label.Breakpoint("bp_main_alloc");

            Label.Checkpoint("bp_main_alloc", "bp_cold_class", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_main_alloc");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_main_alloc");

                // No type object creation (no allocation by this thread) during "bp_main" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "allocMain", "long", "0");
                // Changed by debugger at "bp_main".
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_int", "int", "111");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_int", "int", "121");

                // Types below were never used by program code, each request below is the first access to its type.
                // Default value of these field types is not created by debugger, type object is used (same as for type with
                // static constructor).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdStruct.s_struct", "VSCodeTestStaticFields.ValStruct", "{VSCodeTestStaticFields.ValStruct}");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdArray.s_array", "int[]", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdIntPtr.s_intptr", "IntPtr", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdUIntPtr.s_uintptr", "UIntPtr", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdEnum.s_enum", "VSCodeTestStaticFields.TestEnum", "One");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "NoCctorColdStructAssign.s_struct.field_i", "5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdStructAssign.s_struct.field_i", "int", "5");
                // Default value of these field types is created by debugger, change field (first access to its type).
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "NoCctorColdIntAssign.s_int", "5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdIntAssign.s_int", "int", "5");
                Context.SetExpression(@"__FILE__:__LINE__", frameId, "NoCctorColdStringAssign.s_string", "\"assigned\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdStringAssign.s_string", "string", "\"assigned\"");

                // Static fields not used by program code, change field from variables (static members of variable).
                int instanceStaticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorInstanceOnly");
                Context.EvalVariable(@"__FILE__:__LINE__", instanceStaticsReference, "string", "s_string", "null");
                Context.SetVariable(@"__FILE__:__LINE__", instanceStaticsReference, "s_string", "\"x\"");
                Context.EvalVariable(@"__FILE__:__LINE__", instanceStaticsReference, "string", "s_string", "\"x\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorInstanceOnly.s_string", "string", "\"x\"");
                Context.EvalVariable(@"__FILE__:__LINE__", instanceStaticsReference, "int", "s_int", "0");
                Context.SetVariable(@"__FILE__:__LINE__", instanceStaticsReference, "s_int", "7");
                Context.EvalVariable(@"__FILE__:__LINE__", instanceStaticsReference, "int", "s_int", "7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorInstanceOnly.s_int", "int", "7");
                // Same for static members of struct local variable, array element, boxed struct and base types (same and other
                // module), static fields storage is allocated in all these cases.
                instanceStaticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorLocalStruct");
                Context.SetStaticMembers(@"__FILE__:__LINE__", frameId, instanceStaticsReference, "NoCctorLocalStruct", "s_int", "s_string");
                instanceStaticsReference = Context.GetElementStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorArrayStruct", "[0]");
                Context.SetStaticMembers(@"__FILE__:__LINE__", frameId, instanceStaticsReference, "NoCctorArrayStruct", "s_int", "s_string");
                instanceStaticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorBoxedStruct");
                Context.SetStaticMembers(@"__FILE__:__LINE__", frameId, instanceStaticsReference, "NoCctorBoxedStruct", "s_int", "s_string");
                instanceStaticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "derivedFromUntouchedBase");
                Context.SetStaticMembers(@"__FILE__:__LINE__", frameId, instanceStaticsReference, "NoCctorUntouchedBase", "base_s_int", "base_s_string");
                instanceStaticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "derivedFromOtherModuleBase");
                Context.SetStaticMembers(@"__FILE__:__LINE__", frameId, instanceStaticsReference, "VSCodeTestStaticFieldsLib.NoCctorOtherModuleBase", "lib_s_int", "lib_s_string");

                // Generic type without static constructor.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<int>.s_value", "int", "41");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<int>.s_int", "int", "42");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<int>.t_value", "int", "43");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<int>.t_int", "int", "44");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<string>.s_value", "string", "\"generic_s_value\"");
                // Note, debugger API report thread static field of shared generic instantiation as not available.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<string>.t_value",
                                            "error: The name 'NoCctorGeneric<string>.t_value' does not exist in the current context");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorGeneric<long>.s_value", "long", "0");
                int staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorGeneric");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "s_value", "41");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "t_value", "43");

                // Type with static constructor (type object created here, static constructor run by this thread).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "WithCctorThreadStorage.s_int", "int", "8");

                Context.Continue(@"__FILE__:__LINE__");
            });

            allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy4 = 4;                                                 Label.Breakpoint("bp_cold_class");
            long allocColdClass = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_cold_class", "bp_cold_class_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_cold_class");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_cold_class");

                // Class never used by program code, field type is class never instantiated. Default value is created by debugger
                // (first access to its type).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorColdClass.s_ref", "VSCodeTestStaticFields.NeverInstantiated", "null");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy5 = 5;                                                 Label.Breakpoint("bp_cold_class_alloc");

            Label.Checkpoint("bp_cold_class_alloc", "bp_static_method", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_cold_class_alloc");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_cold_class_alloc");

                // No type object creation (no allocation by this thread) during "bp_cold_class" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "allocColdClass", "long", "0");

                Context.Continue(@"__FILE__:__LINE__");
            });

            NoCctorMethodHolder.StaticMethod();

            System.Threading.Thread workerThread = new System.Threading.Thread(WorkerThread);
            workerThread.Start();
            workerThread.Join();

            System.Threading.Thread workerThread2 = new System.Threading.Thread(WorkerThread2);
            workerThread2.Start();
            workerThread2.Join();

            int dummy3 = 3;                                                 Label.Breakpoint("bp_cctor");

            Label.Checkpoint("bp_cctor", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_cctor");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_cctor");

                // Type with static constructor, that was not run yet: debugger run static constructor before static field access.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.cctor_runs", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "WithCctor.s_int", "int", "5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.cctor_runs", "int", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.generic_cctor_runs", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "WithCctorGeneric<int>.s_int", "int", "6");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.generic_cctor_runs", "int", "1");

                // Base type with static constructor, that was not run yet, accessed through derived type without static constructor.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.base2_cctor_runs", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "DerivedNoCctor2.base2_s_int", "int", "9");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.base2_cctor_runs", "int", "1");

                // Base type with static constructor (already run, since instance was created).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "Counters.base_cctor_runs", "int", "1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "DerivedNoCctor.derived_s_int", "int", "51");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "DerivedNoCctor.base_s_int", "int", "7");
                int staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "derivedNoCctor");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "derived_s_int", "51");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "base_s_int", "7");

                // Static property and method of type without static constructor.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorMembers.s_prop", "int", "51");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorMembers.GetInt()", "int", "52");

                // Constants of type without static constructor.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorConsts.c_int", "int", "3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorConsts.c_string", "string", "\"const\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorConsts.c_enum", "VSCodeTestStaticFields.TestEnum", "Two");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void WorkerThread()
        {
            NoCctor noCctor = new NoCctor();
            NoCctorThreadStorage noCctorThreadStorage = new NoCctorThreadStorage();
            NoCctorWorker.t_long1 = 61;
            NoCctorWorker.t_string = "worker";
            int runtimeMajor = Environment.Version.Major;

            long allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy1 = 1;                                                 Label.Breakpoint("bp_worker");
            long allocWorker = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_worker", "bp_worker_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_worker");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_worker");

                // Thread static fields written by this thread have this thread values, static field is shared by all threads
                // (first access by debugger).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker.t_long1", "long", "61");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker.t_string", "string", "\"worker\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker.t_long0", "long", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker.s_int", "int", "90");

                // Static fields are shared by all threads.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_int", "int", "111");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.s_string", "string", "\"s_string\"");

                // Watch, thread static fields never accessed by this thread have default value.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_int", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_long", "long", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_bool", "bool", "false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_double", "double", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_string", "string", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_object", "object", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_ref", "VSCodeTestStaticFields.RefClass", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_volatile_ref", "VSCodeTestStaticFields.RefClass", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_iface", "VSCodeTestStaticFields.IRef", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_inner", "VSCodeTestStaticFields.Outer.Inner", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_collide", "VSCodeTestStaticFields.ControlInfo", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorStruct.t_int", "int", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long0", "long", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long7", "long", "0");

                // Variables (static members of variable), thread static fields never accessed by this thread have default value.
                int staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorThreadStorage");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "long", "t_long0", "0");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "long", "t_long7", "0");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                                 Label.Breakpoint("bp_worker_alloc");

            Label.Checkpoint("bp_worker_alloc", "bp_worker2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_worker_alloc");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_worker_alloc");

                // No type object creation (no allocation by this thread) during "bp_worker" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "allocWorker", "long", "0");

                // Below, debugger don't create default value of field type (or field can't be changed without storage), type object
                // is used, as for type with static constructor (type object creation don't allocate thread static fields storage).
                // Note, this thread wrote NoCctorWorker thread static fields. Measured on .NET 8.0.26: after this, primitive,
                // reference and array thread static fields of other types have storage on this thread too, but struct field don't
                // (probably, struct thread static field is boxed at first access to type thread static fields by thread).
                // Measured on .NET 9.0.15 and 10.0.12: these fields don't have storage on this thread.
                // For other runtime versions we assume the same as for measured version with same or closest major version.
                int runtimeMajor = int.Parse(Context.GetVariable(@"__FILE__:__LINE__", frameId, "runtimeMajor"));
                bool storage = runtimeMajor <= 8;

                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctor.t_other_module_ref", storage ? "System.Text.StringBuilder null" :
                                    "error: The name 'NoCctor.t_other_module_ref' does not exist in the current context");
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctor.t_struct",
                                    "error: The name 'NoCctor.t_struct' does not exist in the current context");
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctor.t_array", storage ? "int[] null" :
                                    "error: The name 'NoCctor.t_array' does not exist in the current context");
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctor.t_list", storage ? "System.Collections.Generic.List<int> null" :
                                    "error: The name 'NoCctor.t_list' does not exist in the current context");
                // Class never used by program code (first access to its type), type object is created and field is read again.
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctorColdThreadStatic.t_other_module_ref", storage ? "System.Text.StringBuilder null" :
                                    "error: The name 'NoCctorColdThreadStatic.t_other_module_ref' does not exist in the current context");

                // Variables (static members of variable), thread static fields never accessed by this thread have default value.
                int staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctor");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "s_int", "111");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "int", "t_int", "0");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "long", "t_long", "0");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "bool", "t_bool", "false");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "double", "t_double", "0");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "string", "t_string", "null");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "object", "t_object", "null");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, "VSCodeTestStaticFields.RefClass", "t_ref", "null");
                Context.EvalVariable(@"__FILE__:__LINE__", staticsReference, null, "t_struct", "<error>");

                // Change thread static field (storage allocated, if possible).
                Context.SetExpressionResult(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long0", "5", storage,
                                            "'NoCctorThreadStorage.t_long0' cannot be assigned to");
                staticsReference = Context.GetStaticMembersReference(@"__FILE__:__LINE__", frameId, "noCctorThreadStorage");
                Context.SetVariableResult(@"__FILE__:__LINE__", staticsReference, "t_long7", "5", storage, "'t_long7' cannot be assigned to");
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long0", storage ? "long 5" : "long 0");
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "NoCctorThreadStorage.t_long7", storage ? "long 5" : "long 0");

                // Thread static field never accessed by this thread, type with static constructor.
                Context.CheckResult(@"__FILE__:__LINE__", frameId, "WithCctorThreadStorage.t_long0", storage ? "long 0" :
                                    "error: The name 'WithCctorThreadStorage.t_long0' does not exist in the current context");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        // Thread, that never access thread static fields.
        static void WorkerThread2()
        {
            long allocBefore = GC.GetAllocatedBytesForCurrentThread();
            int dummy1 = 1;                                                 Label.Breakpoint("bp_worker2");
            long allocWorker2 = GC.GetAllocatedBytesForCurrentThread() - allocBefore;

            Label.Checkpoint("bp_worker2", "bp_worker2_alloc", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_worker2");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_worker2");

                // Thread static fields never accessed by this thread have default value (first access by debugger).
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker2.t_string", "string", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker2.t_ref", "VSCodeTestStaticFields.RefClass", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker2.t_long0", "long", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctorWorker2.t_long7", "long", "0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_string", "string", "null");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "NoCctor.t_ref", "VSCodeTestStaticFields.RefClass", "null");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                                 Label.Breakpoint("bp_worker2_alloc");

            Label.Checkpoint("bp_worker2_alloc", "bp_cctor", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_worker2_alloc");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp_worker2_alloc");

                // No type object creation (no allocation by this thread) during "bp_worker2" breakpoint stop.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "allocWorker2", "long", "0");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
