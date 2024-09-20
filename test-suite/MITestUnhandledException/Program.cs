using System;
using System.IO;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Runtime.InteropServices;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            // Explicitly enable JMC for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set just-my-code 1").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

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
                if (func.CString == ControlInfo.TestName + ".Program.<Main>d__1.MoveNext()") {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AbortExecution(string caller_trace)
        {
            Assert.Equal(MIResultClass.Done, MIDebugger.Request("-exec-abort").Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
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

                // we don't check exit code here, since Windows and Linux provide different exit code in case of unhandled exception
                return true;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            Assert.Equal(MIResultClass.Exit,
                         MIDebugger.Request("-gdb-exit").Class,
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

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public bool IsValidFrame(MIResult frame, string bpName, int level)
        {
            var lbp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];
            var content = (MITuple)frame.Value;
            if (frame.Variable == "frame" &&
                ((MIConst)content["level"]).Int == level &&
                ((MIConst)content["fullname"]).String.Contains("Program.cs") &&
                ((MIConst)content["line"]).Int == lbp.NumLine)
            {
                return true;
            }

            return false;
        }

        public void TestExceptionStackTrace(string caller_trace,  string[] stacktrace, int num)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var res = MIDebugger.Request("-stack-list-frames");
                Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__");
                var stack = (MIList)res["stack"];

                bool result = true;
                if (win32detect)
                {
                    result = IsValidFrame((MIResult)stack[0], stacktrace[0], 0);
                }
                else
                {
                    for (int i = 0; i < num; i++)
                    {
                        if (!IsValidFrame((MIResult)stack[i], stacktrace[i], i))
                        {
                            result = false;
                            break;
                        }
                    }
                }
                return result;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
            win32detect = RuntimeInformation.IsOSPlatform(OSPlatform.Windows) && IntPtr.Size == 4;
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
        bool win32detect;
    }
}

namespace MITestUnhandledException
{
    class Program
    {
        static void Abc()
        {
            throw new Exception();                                                  Label.Breakpoint("throwexception");
        }

        static async Task Main(string[] args)
        {
            Label.Checkpoint("init", "test_unhandled", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");
            });

            await Task.Yield();
            Abc();                                                                  Label.Breakpoint("callabc");

            Label.Checkpoint("test_unhandled", "finish", (Object context) => {
                Context Context = (Context)context;
                string[] stacktrace = {"throwexception", "callabc"};
                Context.TestExceptionStackTrace(@"__FILE__:__LINE__", stacktrace, 2);
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                // At this point debugger stops at unhandled exception, no reason continue process, abort execution.
                Context.AbortExecution( @"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
