using System;
using System.IO;
using System.Collections.Generic;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Xunit;
using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public static void PrepareStart()
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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = DebuggeeInfo.TargetAssemblyPath;
            // NOTE this code works only with one source file
            launchRequest.arguments.cwd = Directory.GetParent(DebuggeeInfo.SourceFilesPath).FullName;
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success);
        }

        public static void PrepareEnd()
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success);
        }

        public static void WasEntryPointHit()
        {
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                        && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry"));

            foreach (var Event in VSCodeDebugger.EventList) {
                if (VSCodeDebugger.isResponseContainProperty(Event, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(Event, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(Event, "threadId"));
                    break;
                }
            }
        }

        public static void WasExit()
        {
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated"));
        }

        public static void DebuggerExit()
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success);
        }

        public static void AddFuncBreakpoint(string funcName, string Condition = null)
        {
            BreakpointList.Add(new FunctionBreakpoint(funcName, Condition));
        }

        public static void RemoveFuncBreakpoint(string funcName)
        {
            BreakpointList.Remove(BreakpointList.Find(x => x.name == funcName));
        }

        public static void SetFuncBreakpoints()
        {
            SetFunctionBreakpointsRequest setFunctionBreakpointsRequest = new SetFunctionBreakpointsRequest();
            setFunctionBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            Assert.True(VSCodeDebugger.Request(setFunctionBreakpointsRequest).Success);
        }

        public static void WasBreakpointHit(Breakpoint breakpoint)
        {
            string resJSON = VSCodeDebugger.Receive(-1);
            Assert.True(VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                        && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint"));

            foreach (var Event in VSCodeDebugger.EventList) {
                if (VSCodeDebugger.isResponseContainProperty(Event, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(Event, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(Event, "threadId"));
                }
            }

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success);

            Assert.Equal(BreakpointType.Line, breakpoint.Type);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            foreach (var Frame in stackTraceResponse.body.stackFrames) {
                if (Frame.line == lbp.NumLine
                    && Frame.source.name == lbp.FileName
                    // NOTE this code works only with one source file
                    && Frame.source.path == DebuggeeInfo.SourceFilesPath)
                    return;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
        public static List<FunctionBreakpoint> BreakpointList = new List<FunctionBreakpoint>();
    }
}

namespace VSCodeTestFuncBreak
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp1_test", () => {
                Context.PrepareStart();

                Context.AddFuncBreakpoint("funcbrackpoint1");
                Context.AddFuncBreakpoint("funcbrackpoint2(int)");
                Context.AddFuncBreakpoint("Program.funcbrackpoint3()");
                Context.AddFuncBreakpoint("funcbrackpoint4");
                Context.AddFuncBreakpoint("funcbrackpoint6(int)", "i==5");
                Context.AddFuncBreakpoint("VSCodeTestFuncBreak.Program.funcbrackpoint7", "z<10");
                Context.SetFuncBreakpoints();

                // change condition to already setted bp
                Context.RemoveFuncBreakpoint("funcbrackpoint6(int)");
                Context.AddFuncBreakpoint("funcbrackpoint6(int)", "i>10");
                Context.SetFuncBreakpoints();

                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.Continue();
            });

            funcbrackpoint1();
            funcbrackpoint2(5);
            funcbrackpoint3();
            funcbrackpoint4();
            funcbrackpoint5("funcbrackpoint5 test function");
            funcbrackpoint6(5);
            funcbrackpoint7(5);

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        static void funcbrackpoint1()
        {                                                                   Label.Breakpoint("br1");
            Console.WriteLine("funcbrackpoint1 test function");

            Label.Checkpoint("bp1_test", "bp2_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["br1"]);
                Context.Continue();
            });
        }

        static void funcbrackpoint2(int x)
        {                                                                   Label.Breakpoint("br2");
            Console.WriteLine("funcbrackpoint2 test function x=" + x.ToString());

            Label.Checkpoint("bp2_test", "bp3_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["br2"]);
                Context.Continue();
            });
        }

        static void funcbrackpoint3()
        {                                                                   Label.Breakpoint("br3");
            Console.WriteLine("funcbrackpoint3 test function");

            Label.Checkpoint("bp3_test", "bp5_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["br3"]);

                Context.RemoveFuncBreakpoint("funcbrackpoint4");
                Context.AddFuncBreakpoint("VSCodeTestFuncBreak.Program.funcbrackpoint5(string)");
                Context.SetFuncBreakpoints();

                Context.Continue();
            });
        }

        static void funcbrackpoint4()
        {
            Console.WriteLine("funcbrackpoint4 test function");
        }

        static void funcbrackpoint5(string text)
        {                                                                   Label.Breakpoint("br5");
            Console.WriteLine(text);

            Label.Checkpoint("bp5_test", "bp7_test", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["br5"]);
                Context.Continue();
            });
        }

        static void funcbrackpoint6(int i)
        {
            Console.WriteLine("i=" + i.ToString());
        }

        static void funcbrackpoint7(int z)
        {                                                                   Label.Breakpoint("br7");
            Console.WriteLine("z=" + z.ToString());

            Label.Checkpoint("bp7_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["br7"]);
                Context.Continue();
            });
        }
    }
}
