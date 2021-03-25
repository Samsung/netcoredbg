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

        public void AddFuncBreakpoint(string funcName, string Condition = null)
        {
            BreakpointList.Add(new FunctionBreakpoint(funcName, Condition));
        }

        public void RemoveFuncBreakpoint(string funcName)
        {
            BreakpointList.Remove(BreakpointList.Find(x => x.name == funcName));
        }

        public void SetFuncBreakpoints(string caller_trace)
        {
            SetFunctionBreakpointsRequest setFunctionBreakpointsRequest = new SetFunctionBreakpointsRequest();
            setFunctionBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            Assert.True(VSCodeDebugger.Request(setFunctionBreakpointsRequest).Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == lbp.NumLine
                    && Frame.source.name == lbp.FileName
                    // NOTE this code works only with one source file
                    && Frame.source.path == ControlInfo.SourceFilesPath)
                    return;
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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
        List<FunctionBreakpoint> BreakpointList = new List<FunctionBreakpoint>();
    }
}

namespace VSCodeTestFuncBreak
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp1_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                Context.AddFuncBreakpoint("funcbrackpoint1");
                Context.AddFuncBreakpoint("funcbrackpoint2(int)");
                Context.AddFuncBreakpoint("Program.funcbrackpoint3()");
                Context.AddFuncBreakpoint("funcbrackpoint4");
                Context.AddFuncBreakpoint("funcbrackpoint6(int)", "i==5");
                Context.AddFuncBreakpoint("VSCodeTestFuncBreak.Program.funcbrackpoint7", "z<10");
                Context.SetFuncBreakpoints(@"__FILE__:__LINE__");

                // change condition to already setted bp
                Context.RemoveFuncBreakpoint("funcbrackpoint6(int)");
                Context.AddFuncBreakpoint("funcbrackpoint6(int)", "i>10");
                Context.SetFuncBreakpoints(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            funcbrackpoint1();
            funcbrackpoint2(5);
            funcbrackpoint3();
            funcbrackpoint4();
            funcbrackpoint5("funcbrackpoint5 test function");
            funcbrackpoint6(5);
            funcbrackpoint7(5);

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void funcbrackpoint1()
        {                                                                   Label.Breakpoint("br1");
            Console.WriteLine("funcbrackpoint1 test function");

            Label.Checkpoint("bp1_test", "bp2_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "br1");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void funcbrackpoint2(int x)
        {                                                                   Label.Breakpoint("br2");
            Console.WriteLine("funcbrackpoint2 test function x=" + x.ToString());

            Label.Checkpoint("bp2_test", "bp3_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "br2");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void funcbrackpoint3()
        {                                                                   Label.Breakpoint("br3");
            Console.WriteLine("funcbrackpoint3 test function");

            Label.Checkpoint("bp3_test", "bp5_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "br3");

                Context.RemoveFuncBreakpoint("funcbrackpoint4");
                Context.AddFuncBreakpoint("VSCodeTestFuncBreak.Program.funcbrackpoint5(string)");
                Context.SetFuncBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void funcbrackpoint4()
        {
            Console.WriteLine("funcbrackpoint4 test function");
        }

        static void funcbrackpoint5(string text)
        {                                                                   Label.Breakpoint("br5");
            Console.WriteLine(text);

            Label.Checkpoint("bp5_test", "bp7_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "br5");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void funcbrackpoint6(int i)
        {
            Console.WriteLine("i=" + i.ToString());
        }

        static void funcbrackpoint7(int z)
        {                                                                   Label.Breakpoint("br7");
            Console.WriteLine("z=" + z.ToString());

            Label.Checkpoint("bp7_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "br7");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
