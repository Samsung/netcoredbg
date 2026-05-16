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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);

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
            Assert.True(VSCodeDebugger.Request(launchRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void PrepareEnd(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }


        public void AddFuncBreakpoint(string funcName, string Condition = null, string LogMessage = null)
        {
            FuncBreakpointList.Add(new FunctionBreakpoint(funcName, Condition)
            {
                logMessage = LogMessage
            });
        }

        public void SetFuncBreakpoints(string caller_trace)
        {
            SetFunctionBreakpointsRequest setFunctionBreakpointsRequest = new SetFunctionBreakpointsRequest();
            setFunctionBreakpointsRequest.arguments.breakpoints.AddRange(FuncBreakpointList);
            Assert.True(VSCodeDebugger.Request(setFunctionBreakpointsRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddBreakpoint(string caller_trace, string bpName, string Condition = null, string LogMessage = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition)
            {
                logMessage = LogMessage
            });
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
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void WereAllLogPointsHit(string caller_trace, List<string> logMessages)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "output")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "category", "console"))
                {
                    var rawOutput = VSCodeDebugger.GetResponsePropertyValue(resJSON, "output");
                    if (rawOutput is string output)
                        logMessages.Remove(output);
                }
                return logMessages.Count == 0;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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
        List<FunctionBreakpoint> FuncBreakpointList = new List<FunctionBreakpoint>();
    }
}

namespace VSCodeTestLogPoint
{
    class Program
    {
        static void testfunc()
        {                                                                       Label.Breakpoint("lp_func");
            Console.WriteLine("A logpoint is set on this testfunc");
        }

        static void testfunc_with_param(string param)
        {                                                                       Label.Breakpoint("lp_func_param");
            Console.WriteLine("A logpoint is set on this testfunc with param: " + param);
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp1", LogMessage: "Logpoint hit");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp2", LogMessage: "Variable value: {myVar}");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp3", LogMessage: "abc \\{escaped}");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp4", LogMessage: "not {closed");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp5", LogMessage: "not closed}");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp6", LogMessage: "not closed\\}");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "lp7", LogMessage: "abc {str} abc");

                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.AddFuncBreakpoint("testfunc", LogMessage: "Logpoint hit in func");
                Context.AddFuncBreakpoint("testfunc_with_param", LogMessage: "Logpoint hit in func with param: {param}");

                Context.SetFuncBreakpoints(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WereAllLogPointsHit(@"__FILE__:__LINE__", new List<string> {
                    "Logpoint hit\n",
                    "Variable value: 109023\n",
                    "abc {escaped}\n",
                    "not {closed\n",
                    "not closed}\n",
                    "not closed}\n",
                    "abc \"some string\" abc\n",
                    "Logpoint hit in func\n",
                    "Logpoint hit in func with param: \"parameter value\"\n"
                });
            });

            System.Threading.Thread.Sleep(1000);

            Console.WriteLine("Logpoint \"lp1\" is set here"); Label.Breakpoint("lp1");

            int myVar = 109023;

            Console.WriteLine("Logpoint \"lp2\" is set here"); Label.Breakpoint("lp2");

            Console.WriteLine("Logpoint \"lp3\" is set here"); Label.Breakpoint("lp3");

            Console.WriteLine("Logpoint \"lp4\" is set here"); Label.Breakpoint("lp4");

            Console.WriteLine("Logpoint \"lp5\" is set here"); Label.Breakpoint("lp5");

            Console.WriteLine("Logpoint \"lp6\" is set here"); Label.Breakpoint("lp6");

            string str = "some string";

            Console.WriteLine("Logpoint \"lp7\" is set here"); Label.Breakpoint("lp7");

            testfunc();

            testfunc_with_param("parameter value");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
