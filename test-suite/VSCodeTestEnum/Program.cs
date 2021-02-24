using System;
using System.IO;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

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
            launchRequest.arguments.cwd = "";
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
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void WasExit()
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

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static void DebuggerExit()
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success);
        }

        public static void AddBreakpoint(string bpName, string Condition = null)
        {
            Breakpoint bp = DebuggeeInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition));
            BreakpointLines.Add(lbp.NumLine);
        }

        public static void SetBreakpoints()
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = BreakpointSourceName;
            // NOTE this code works only with one source file
            setBreakpointsRequest.arguments.source.path = DebuggeeInfo.SourceFilesPath;
            setBreakpointsRequest.arguments.lines.AddRange(BreakpointLines);
            setBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            setBreakpointsRequest.arguments.sourceModified = false;
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success);
        }

        public static void WasBreakpointHit(Breakpoint breakpoint)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            if (!VSCodeDebugger.IsEventReceived(filter))
                throw new NetcoreDbgTestCore.ResultNotSuccessException();

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

        public static Int64 DetectFrameId(Breakpoint breakpoint)
        {
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
                    return Frame.id;
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static int GetVariablesReference(Int64 frameId, string ScopeName)
        {
            ScopesRequest scopesRequest = new ScopesRequest();
            scopesRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(scopesRequest);
            Assert.True(ret.Success);

            ScopesResponse scopesResponse =
                JsonConvert.DeserializeObject<ScopesResponse>(ret.ResponseStr);

            foreach (var Scope in scopesResponse.body.scopes) {
                if (Scope.name == ScopeName) {
                    return Scope.variablesReference == null ? 0 : (int)Scope.variablesReference;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        public static VariablesResponse GetLocalVariables(Int64 frameId)
        {
            int variablesReference = Context.GetVariablesReference(frameId, "Locals");

            VariablesRequest variablesRequest = new VariablesRequest();
            variablesRequest.arguments.variablesReference = variablesReference;
            var ret = VSCodeDebugger.Request(variablesRequest);
            Assert.True(ret.Success);

           return JsonConvert.DeserializeObject<VariablesResponse>(ret.ResponseStr);
        }

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        public static void CheckEnum(VariablesResponse variablesResponse, string VarName, string ExpectedResult)
        {
            foreach (var Variable in variablesResponse.body.variables) {
                if (Variable.name == VarName) {
                    Assert.True(Variable.value == ExpectedResult);
                    return;
                }
            }

            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
        // NOTE this code works only with one source file
        public static string BreakpointSourceName;
        public static List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        public static List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestEnum
{
    class Program
    {

        public enum enum1
        {
            read = 1,
            write = 2,
            append = 3
        }

        public enum enum2
        {
            append = 3,
            write = 2,
            read = 1,
            None = 0 // legit code
        }

        [Flags]
        public enum enum3
        {
            append = 4,
            write = 2,
            read = 1
        }

        [Flags]
        public enum enum4
        {
            read = 1,
            write = 2,
            append = 4,
            None = 0 // legit code
        }

        [Flags]
        public enum enum5
        {
            read = 1,
            write = 2,
            append = 3  // check for wrong code logic, not powers of two
        }

        [Flags]
        public enum enum6
        {
            append = 3, // check for wrong code logic, not powers of two
            write = 2,
            read = 1
        }

        public enum enum7 : byte
        {
            read = 1,
            write = 2
        }

        public enum enum8 : ushort
        {
            read = 1,
            write = 2
        }

        public enum enum9 : uint
        {
            read = 1,
            write = 2
        }

        public enum enum10 : ulong
        {
            read = 1,
            write = 2
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", () => {
                Context.PrepareStart();
                Context.AddBreakpoint("bp");
                Context.SetBreakpoints();
                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.Continue();
            });

            // Note, we test for same behaviour as MS vsdbg have.

            enum1 enum1_test0 = (enum1)0;                 // check for wrong code logic, result = 0 - out of enumeration
            enum1 enum1_test1 = enum1.read;
            enum1 enum1_test2 = enum1.write;
            enum1 enum1_test3 = enum1.append;
            enum1 enum1_test4 = enum1.read & enum1.write; // check for wrong code logic, result = 0
            enum1 enum1_test5 = enum1.read | enum1.write; // check for wrong code logic, result = append - not flags enumeration can't be OR-ed
            enum1 enum1_test6 = (enum1)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum2 enum2_test0 = (enum2)0;                 // result = None
            enum2 enum2_test1 = enum2.read;
            enum2 enum2_test2 = enum2.write;
            enum2 enum2_test3 = enum2.append;
            enum2 enum2_test4 = enum2.read & enum2.write; // check for wrong code logic, result = None
            enum2 enum2_test5 = enum2.read | enum2.write; // check for wrong code logic, result = append - not flags enumeration can't be OR-ed
            enum2 enum2_test6 = (enum2)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum3 enum3_test0 = (enum3)0;                 // check for wrong code logic, result = 0 - out of enumeration
            enum3 enum3_test1 = enum3.read;
            enum3 enum3_test2 = enum3.write;
            enum3 enum3_test3 = enum3.append;
            enum3 enum3_test4 = enum3.read & enum3.write; // check for wrong code logic, result = 0 - out of enumeration
            enum3 enum3_test5 = enum3.read | enum3.write | enum3.append; // check that debugger care about enum sequence in case of flags attribute, result = read | write | append
            enum3 enum3_test6 = (enum3)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum4 enum4_test0 = (enum4)0;                 // result = None
            enum4 enum4_test1 = enum4.read;
            enum4 enum4_test2 = enum4.write;
            enum4 enum4_test3 = enum4.append;
            enum4 enum4_test4 = enum4.read & enum4.write; // check for wrong code logic, result = None
            enum4 enum4_test5 = enum4.read | enum4.write | enum4.append; // check that debugger care about enum sequence in case of flags attribute, result = read | write | append
            enum4 enum4_test6 = (enum4)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum5 enum5_test1 = enum5.append;             // check that debugger care about enum sequence in case of flags attribute, result = append
            enum5 enum5_test2 = enum5.read | enum5.write; // check that debugger care about enum sequence in case of flags attribute, result = append
            enum5 enum5_test3 = enum5.write | enum5.read; // check that debugger care about enum sequence in case of flags attribute, result = append

            enum6 enum6_test1 = enum6.append;             // check that debugger care about enum sequence in case of flags attribute, result = append
            enum6 enum6_test2 = enum6.read | enum6.write; // check that debugger care about enum sequence in case of flags attribute, result = append
            enum6 enum6_test3 = enum6.write | enum6.read; // check that debugger care about enum sequence in case of flags attribute, result = append

            enum7 enum7_test1 = enum7.read;
            enum7 enum7_test2 = enum7.write;

            enum8 enum8_test1 = enum8.read;
            enum8 enum8_test2 = enum8.write;

            enum9 enum9_test1 = enum9.read;
            enum9 enum9_test2 = enum9.write;

            enum10 enum10_test1 = enum10.read;
            enum10 enum10_test2 = enum10.write;

            Console.WriteLine("A breakpoint \"bp\" is set on this line"); Label.Breakpoint("bp");

            Label.Checkpoint("values_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp"]);

                Int64 frameId = Context.DetectFrameId(DebuggeeInfo.Breakpoints["bp"]);
                VariablesResponse variablesResponse = Context.GetLocalVariables(frameId);

                Context.CheckEnum(variablesResponse, "enum1_test0", "0");
                Context.CheckEnum(variablesResponse, "enum1_test1", "read");
                Context.CheckEnum(variablesResponse, "enum1_test2", "write");
                Context.CheckEnum(variablesResponse, "enum1_test3", "append");
                Context.CheckEnum(variablesResponse, "enum1_test4", "0");
                Context.CheckEnum(variablesResponse, "enum1_test5", "append");
                Context.CheckEnum(variablesResponse, "enum1_test6", "101");

                Context.CheckEnum(variablesResponse, "enum2_test0", "None");
                Context.CheckEnum(variablesResponse, "enum2_test1", "read");
                Context.CheckEnum(variablesResponse, "enum2_test2", "write");
                Context.CheckEnum(variablesResponse, "enum2_test3", "append");
                Context.CheckEnum(variablesResponse, "enum2_test4", "None");
                Context.CheckEnum(variablesResponse, "enum2_test5", "append");
                Context.CheckEnum(variablesResponse, "enum2_test6", "101");

                Context.CheckEnum(variablesResponse, "enum3_test0", "0");
                Context.CheckEnum(variablesResponse, "enum3_test1", "read");
                Context.CheckEnum(variablesResponse, "enum3_test2", "write");
                Context.CheckEnum(variablesResponse, "enum3_test3", "append");
                Context.CheckEnum(variablesResponse, "enum3_test4", "0");
                Context.CheckEnum(variablesResponse, "enum3_test5", "read | write | append");
                Context.CheckEnum(variablesResponse, "enum3_test6", "101");

                Context.CheckEnum(variablesResponse, "enum4_test0", "None");
                Context.CheckEnum(variablesResponse, "enum4_test1", "read");
                Context.CheckEnum(variablesResponse, "enum4_test2", "write");
                Context.CheckEnum(variablesResponse, "enum4_test3", "append");
                Context.CheckEnum(variablesResponse, "enum4_test4", "None");
                Context.CheckEnum(variablesResponse, "enum4_test5", "read | write | append");
                Context.CheckEnum(variablesResponse, "enum4_test6", "101");

                Context.CheckEnum(variablesResponse, "enum5_test1", "append");
                Context.CheckEnum(variablesResponse, "enum5_test2", "append");
                Context.CheckEnum(variablesResponse, "enum5_test3", "append");

                Context.CheckEnum(variablesResponse, "enum6_test1", "append");
                Context.CheckEnum(variablesResponse, "enum6_test2", "append");
                Context.CheckEnum(variablesResponse, "enum6_test3", "append");

                Context.CheckEnum(variablesResponse, "enum7_test1", "read");
                Context.CheckEnum(variablesResponse, "enum7_test2", "write");

                Context.CheckEnum(variablesResponse, "enum8_test1", "read");
                Context.CheckEnum(variablesResponse, "enum8_test2", "write");

                Context.CheckEnum(variablesResponse, "enum9_test1", "read");
                Context.CheckEnum(variablesResponse, "enum9_test2", "write");

                Context.CheckEnum(variablesResponse, "enum10_test1", "read");
                Context.CheckEnum(variablesResponse, "enum10_test2", "write");    

                Context.Continue();
            });

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
