using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

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
            launchRequest.arguments.name = ".NET Core Launch (web)";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";

            string AbsolutePathToAssembly = Path.GetFullPath(ControlInfo.TargetAssemblyPath);
            launchRequest.arguments.program = Path.GetFileName(AbsolutePathToAssembly);
            string targetAssemblyPath = Path.GetFileName(AbsolutePathToAssembly);
            int subLength = AbsolutePathToAssembly.Length - targetAssemblyPath.Length;
            string dllPath = AbsolutePathToAssembly.Substring(0, subLength);
            launchRequest.arguments.cwd = dllPath;

            launchRequest.arguments.env = new Dictionary<string, string>();
            launchRequest.arguments.env.Add("ASPNETCORE_ENVIRONMENT", "Development");
            launchRequest.arguments.env.Add("ASPNETCORE_URLS", "https://localhost:25001");
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
                    Assert.Equal(Type, Variable.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(Value, Variable.value, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestEnv
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "env_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            string EnvA = Environment.GetEnvironmentVariable("ASPNETCORE_ENVIRONMENT");
            string EnvB = Environment.GetEnvironmentVariable("ASPNETCORE_URLS");

            Console.WriteLine("Env A = " + EnvA + " Env B = " + EnvB); Label.Breakpoint("bp");

            Label.Checkpoint("env_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "bp");

                int variablesReference_Locals = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                int variablesReference = Context.GetVariablesReference(@"__FILE__:__LINE__", frameId, "Locals");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "EnvA", "\"Development\"");
                Context.EvalVariable(@"__FILE__:__LINE__", variablesReference, "string", "EnvB", "\"https://localhost:25001\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
