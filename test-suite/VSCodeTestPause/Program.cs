using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Xunit;
using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public static void Prepare()
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

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        public static void Pause()
        {
            PauseRequest pauseRequest = new PauseRequest();
            pauseRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(pauseRequest).Success);
        }

        public static void WasPaused()
        {
            foreach (var Event in VSCodeDebugger.EventList) {
                if (VSCodeDebugger.isResponseContainProperty(Event, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(Event, "reason", "pause")) {
                    return;
                }
            }
            throw new NetcoreDbgTestCore.ResultNotSuccessException();
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
    }
}

namespace VSCodeTestPause
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "pause_test", () => {
                Context.Prepare();
                Context.WasEntryPointHit();
                Context.Continue();

                Context.Pause();
            });

            System.Threading.Thread.Sleep(3000);

            Label.Checkpoint("pause_test", "finish", () => {
                Context.WasPaused();
                Context.Continue();
            });

            Label.Checkpoint("finish", "", () => {;
                Context.WasExit();
                Context.DebuggerExit();
            });
        }
    }
}
