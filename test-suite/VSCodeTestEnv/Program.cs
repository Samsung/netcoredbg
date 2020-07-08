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
            launchRequest.arguments.name = ".NET Core Launch (web)";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";

            launchRequest.arguments.program = Path.GetFileName(DebuggeeInfo.TargetAssemblyPath);
            string targetAssemblyPath = Path.GetFileName(DebuggeeInfo.TargetAssemblyPath);
            int subLength = DebuggeeInfo.TargetAssemblyPath.Length - targetAssemblyPath.Length;
            string dllPath = DebuggeeInfo.TargetAssemblyPath.Substring(0, subLength);
            launchRequest.arguments.cwd = dllPath;

            launchRequest.arguments.env = new Dictionary<string, string>();
            launchRequest.arguments.env.Add("ASPNETCORE_ENVIRONMENT", VALUE_A);
            launchRequest.arguments.env.Add("ASPNETCORE_URLS", VALUE_B);
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

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;

        public const string VALUE_A = "Development";
        public const string VALUE_B = "https://localhost:25001";
    }
}

namespace VSCodeTestEnv
{
    class Program
    {
        public static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", () => {
                Context.PrepareStart();
                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.Continue();
            });

            // Begin user code
            user_code();
            // End user code

            Label.Checkpoint("finish", "", () => {
                Context.WasExit();
                Context.DebuggerExit();
            });
        }

        public static void user_code()
        {
            var read_a = Environment.GetEnvironmentVariable("ASPNETCORE_ENVIRONMENT");
            var read_b = Environment.GetEnvironmentVariable("ASPNETCORE_URLS");

            // xunit Asserts() has a wrong behavior under Tizen devices
            if (!String.Equals(Context.VALUE_A, read_a) || !String.Equals(Context.VALUE_B, read_b))
                throw new NotImplementedException("TEST FAILED");
        }
    }
}
