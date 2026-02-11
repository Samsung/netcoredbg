using System;
using System.IO;
using System.Diagnostics;
using System.Collections.Generic;
using System.Runtime.InteropServices;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
            ShouldRun = RuntimeInformation.IsOSPlatform(OSPlatform.Windows);
        }

        public bool ShouldRun { get; }

        public void Run(string caller_trace)
        {
            if (!ShouldRun)
                return;

            ResolveTargetPaths(caller_trace);
            BuildTarget(caller_trace);
            Initialize(caller_trace);
            Launch(caller_trace);
            SetBreakpoints(caller_trace);
            ConfigurationDone(caller_trace);

            WaitBreakpointAndAssertStack(caller_trace, ConstructorBreakpointLine, "constructor stop");
            Continue(caller_trace);

            WaitBreakpointAndAssertStack(caller_trace, LoadBreakpointLine, "load-event stop");
            Continue(caller_trace);
        }

        public void WaitExit(string caller_trace)
        {
            if (!ShouldRun)
                return;

            bool wasExited = false;
            int? exitCode = null;
            bool wasTerminated = false;

            Func<string, bool> filter = (resJSON) =>
            {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "exited"))
                {
                    wasExited = true;
                    ExitedEvent exitedEvent = JsonConvert.DeserializeObject<ExitedEvent>(resJSON);
                    exitCode = exitedEvent.body.exitCode;
                }
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated"))
                {
                    wasTerminated = true;
                }
                return wasExited && exitCode == 0 && wasTerminated;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        public void AbortExecution(string caller_trace)
        {
            if (!ShouldRun)
                return;

            TerminateRequest terminateRequest = new TerminateRequest();
            terminateRequest.arguments = new TerminateArguments();
            terminateRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(terminateRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            if (!ShouldRun)
                return;

            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void ResolveTargetPaths(string caller_trace)
        {
            string[] sourceFiles = ControlInfo.SourceFilesPath.Split(';', StringSplitOptions.RemoveEmptyEntries);
            Assert.True(sourceFiles.Length > 0, @"__FILE__:__LINE__" + "\n" + caller_trace);

            string harnessFilePath = sourceFiles[0];
            string harnessDir = Path.GetDirectoryName(harnessFilePath);
            Assert.True(!String.IsNullOrEmpty(harnessDir), @"__FILE__:__LINE__" + "\n" + caller_trace);

            string suiteRoot = Path.GetFullPath(Path.Combine(harnessDir, ".."));
            targetProjectDir = Path.Combine(suiteRoot, "TestAppWinFormsStackTrace");
            targetProjectPath = Path.Combine(targetProjectDir, "TestAppWinFormsStackTrace.csproj");
            targetProgramPath = Path.Combine(targetProjectDir, "bin", "Debug", "net10.0-windows", "TestAppWinFormsStackTrace.dll");
            targetSourcePath = Path.Combine(targetProjectDir, "Form1.cs");
        }

        private void BuildTarget(string caller_trace)
        {
            var process = new Process();
            process.StartInfo.FileName = "dotnet";
            process.StartInfo.Arguments = $"build \"{targetProjectPath}\" -c Debug";
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.CreateNoWindow = true;

            Assert.True(process.Start(), @"__FILE__:__LINE__" + "\n" + caller_trace);
            process.WaitForExit();

            if (process.ExitCode != 0)
            {
                throw new ResultNotSuccessException(@"__FILE__:__LINE__" + "\n" + caller_trace);
            }

            Assert.True(File.Exists(targetProgramPath), @"__FILE__:__LINE__" + "\n" + caller_trace);
            Assert.True(File.Exists(targetSourcePath), @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void Initialize(string caller_trace)
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
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void Launch(string caller_trace)
        {
            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (winforms stackTrace)";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = targetProgramPath;
            launchRequest.arguments.cwd = targetProjectDir;
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = false;
            launchRequest.arguments.justMyCode = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void SetBreakpoints(string caller_trace)
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = "Form1.cs";
            setBreakpointsRequest.arguments.source.path = targetSourcePath;
            setBreakpointsRequest.arguments.lines.Add(ConstructorBreakpointLine);
            setBreakpointsRequest.arguments.lines.Add(LoadBreakpointLine);
            setBreakpointsRequest.arguments.breakpoints.Add(new SourceBreakpoint(ConstructorBreakpointLine));
            setBreakpointsRequest.arguments.breakpoints.Add(new SourceBreakpoint(LoadBreakpointLine));
            setBreakpointsRequest.arguments.sourceModified = false;

            var ret = VSCodeDebugger.Request(setBreakpointsRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void ConfigurationDone(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void WaitBreakpointAndAssertStack(string caller_trace, int expectedLine, string frameHint)
        {
            Func<string, bool> filter = (resJSON) =>
            {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint"))
                {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__" + "\n" + caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__" + "\n" + caller_trace + "\n" + frameHint);

            StackTraceResponse stackTraceResponse = JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);
            Assert.True(stackTraceResponse.body.stackFrames.Count > 0, @"__FILE__:__LINE__" + "\n" + caller_trace);
            Assert.Equal(expectedLine, stackTraceResponse.body.stackFrames[0].line, @"__FILE__:__LINE__" + "\n" + caller_trace);
            Assert.Equal(targetSourcePath, stackTraceResponse.body.stackFrames[0].source.path, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success, @"__FILE__:__LINE__" + "\n" + caller_trace);
        }

        private const int ConstructorBreakpointLine = 11;
        private const int LoadBreakpointLine = 16;

        private readonly ControlInfo ControlInfo;
        private readonly VSCodeDebugger VSCodeDebugger;
        private int threadId = -1;
        private string targetProjectDir;
        private string targetProjectPath;
        private string targetProgramPath;
        private string targetSourcePath;
    }
}

namespace VSCodeTestStackTraceWinForms
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", (Object context) =>
            {
                Context Context = (Context)context;
                Context.Run(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) =>
            {
                Context Context = (Context)context;
                Context.AbortExecution(@"__FILE__:__LINE__");
                Context.WaitExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
