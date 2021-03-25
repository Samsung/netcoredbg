using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-file-exec-and-symbols " + ControlInfo.CorerunPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-exec-arguments " + ControlInfo.TargetAssemblyPath).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-run").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        bool IsStoppedEvent(MIOutOfBandRecord record)
        {
            if (record.Type != MIOutOfBandRecordType.Async) {
                return false;
            }

            var asyncRecord = (MIAsyncRecord)record;

            if (asyncRecord.Class != MIAsyncRecordClass.Exec ||
                asyncRecord.Output.Class != MIAsyncOutputClass.Stopped) {
                return false;
            }

            return true;
        }

        public void WasEntryPointHit(string caller_trace)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "entry-point-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var func = (MIConst)frame["func"];
                if (func.CString == ControlInfo.TestName + ".Program.Main()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace, int ExitCode)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "exited") {
                    return false;
                }

                var exitCode = (MIConst)output["exit-code"];

                if (exitCode.CString == ExitCode.ToString()) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
    }
}

namespace MITestExitCode
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

        static int Main(string[] args)
        {
            // first checkpoint (initialization) must provide "init" as id
            Label.Checkpoint("init", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
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

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__", 3);
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });

            return 0;
        }
    }
}
