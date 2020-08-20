using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Xunit;
using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    // Context includes methods and constants which
    // will be move to debugger API
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

        public static void WasExit(int ExitCode)
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
                if (wasExited && exitCode == ExitCode && wasTerminated)
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

        public static void Continue()
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success);
        }

        static VSCodeDebugger VSCodeDebugger = new VSCodeDebugger();
        static int threadId = -1;
    }
}

namespace VSCodeTestExitCode
{
    class Program
    {
        [DllImport("libc")]
        static extern void exit(int status);

        [DllImport("libc")]
        static extern void _exit(int status);

        [DllImport("libc")]
        static extern int kill(int pid, int sig);

        [DllImport("kernel32.dll")]
        static extern void ExitProcess(uint uExitCode);

        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        static extern bool TerminateProcess(IntPtr hProcess, uint uExitCode);

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", () => {
                Context.PrepareStart();
                Context.PrepareEnd();
                Context.WasEntryPointHit();
                Context.Continue();
            });

            // TODO as soon, as netcoredbg will be able restart debuggee process, implement all tests

            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                //Console.WriteLine("Test TerminateProcess()");
                //ExitProcess(3);

                Console.WriteLine("Test TerminateProcess()");
                TerminateProcess(Process.GetCurrentProcess().Handle, 3);
            }
            else if (RuntimeInformation.IsOSPlatform(OSPlatform.Linux))
            {
                //Console.WriteLine("Test exit()");
                //exit(3);

                Console.WriteLine("Test _exit()");
                _exit(3);

                //int PID = Process.GetCurrentProcess().Id;
                //Console.WriteLine("Test SIGABRT, process Id = " + PID);
                //kill(PID, 6); // SIGABRT
            }

            //Console.WriteLine("Test return 3");
            //return 3;

            //Console.WriteLine("Test throw new System.Exception()");
            //throw new System.Exception();

            Label.Checkpoint("finish", "", () => {
                Context.WasExit(3);
                Context.DebuggerExit();
            });
        }
    }
}
