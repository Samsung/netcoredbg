using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;

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
            launchRequest.arguments.justMyCode = true; // Explicitly enable JMC for this test.
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

        public void RemoveBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;

            BreakpointList.Remove(BreakpointList.Find(x => x.line == lbp.NumLine));
            BreakpointLines.Remove(lbp.NumLine);
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

namespace VSCodeTestBreakpoint
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp1_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp3");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("A breakpoint \"bp1\" is set on this line"); Label.Breakpoint("bp1");

            Label.Checkpoint("bp1_test", "bp3_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp1");

                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp2");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("A breakpoint \"bp2\" is set on this line and removed"); Label.Breakpoint("bp2");

            Console.WriteLine("A breakpoint \"bp3\" is set on this line"); Label.Breakpoint("bp3");

            Label.Checkpoint("bp3_test", "bp5_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp3");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp4", "i==5");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp5", "i>10 || z==0");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                // change condition to already setted bp
                Context.RemoveBreakpoint(@"__FILE__:__LINE__", "bp4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "bp4", "i>10");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int i = 5;
            Console.WriteLine("A breakpoint \"bp4\" is set on this line, i=" + i.ToString()); Label.Breakpoint("bp4");

            int z = 0;
            Console.WriteLine("A breakpoint \"bp5\" is set on this line, z=" + z.ToString()); Label.Breakpoint("bp5");

            Label.Checkpoint("bp5_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp5");

                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR1");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR2");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR3");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR4");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR5");
                Context.SetBreakpoints(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            test_attr_func1();
            test_attr_func2();
            test_attr_func3();
            ctest_attr1.test_func();
            ctest_attr2.test_func();

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        [DebuggerStepThroughAttribute()]
        static void test_attr_func1()
        {                                                           Label.Breakpoint("BREAK_ATTR1");
        }

        [DebuggerNonUserCodeAttribute()]
        static void test_attr_func2()
        {                                                           Label.Breakpoint("BREAK_ATTR2");
        }

        [DebuggerHiddenAttribute()]
        static void test_attr_func3()
        {                                                           Label.Breakpoint("BREAK_ATTR3");
        }
    }

    [DebuggerStepThroughAttribute()]
    class ctest_attr1
    {
        public static void test_func()
        {                                                           Label.Breakpoint("BREAK_ATTR4");
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static void test_func()
        {                                                           Label.Breakpoint("BREAK_ATTR5");
        }
    }
}
