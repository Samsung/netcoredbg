using System;
using System.IO;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;
using System.Collections.Generic;

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

        public void WasBreakpointWithIdHit(string caller_trace, string bpName, string id)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var bkptno = (MIConst)output["bkptno"];
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    bkptno.CString == id &&
                    line == bp.NumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasFuncBreakpointWithIdHit(string caller_trace, string func_name, string id)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "breakpoint-hit") {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var funcName = ((MIConst)frame["func"]).CString;
                var bkptno = ((MIConst)output["bkptno"]).CString;

                if (funcName.EndsWith(func_name) &&
                    bkptno == id) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WereAllLogPointsHit(string caller_trace, List<string> expectedOutput)
        {
            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (record.Type != MIOutOfBandRecordType.Async)
                    return false;

                var asyncRecord = (MIAsyncRecord)record;

                if (asyncRecord.Class != MIAsyncRecordClass.Notify)
                    return false;

                var output = asyncRecord.Output;

                if (!output.TryGetValue("text", out var text))
                    return false;

                var cstr = ((MIConst)text).CString;

                expectedOutput.Remove(cstr);

                return expectedOutput.Count == 0;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

                var exitCode = (MIConst)output["exit-code"];

                if (exitCode.CString == "0") {
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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        public ControlInfo ControlInfo { get; private set; }
        public MIDebugger MIDebugger { get; private set; }
    }
}

namespace MITestLogPoint
{
    class Program
    {
        static void testfunc()
        {                                                                       Label.Breakpoint("lp_func");
            Console.WriteLine("testfunc");
        }

        static void testfunc_with_param(string param)
        {                                                                       Label.Breakpoint("lp_func_param");
            Console.WriteLine("testfunc_with_param");
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");

                Context.MIDebugger.Request($"3-dprintf-insert -f {Context.ControlInfo.Breakpoints["lp1"]} \"lp1 log point\"");

                Context.MIDebugger.Request($"4-dprintf-insert -f {Context.ControlInfo.Breakpoints["lp2"]} \"value of x = %s\" x");

                Context.MIDebugger.Request($"5-dprintf-insert -f {Context.ControlInfo.Breakpoints["lp3"]} \"value of y = %s\" y");

                Context.MIDebugger.Request($"6-dprintf-insert -f {Context.ControlInfo.Breakpoints["lp_func"]} \"log point in testfunc\"");

                Context.MIDebugger.Request($"7-dprintf-insert -f {Context.ControlInfo.Breakpoints["lp_func_param"]} \"log point in testfunc_with_param, param = %s\" param");

                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WereAllLogPointsHit(@"__FILE__:__LINE__", new List<string> {
                    "lp1 log point\\n",
                    "value of x = 42\\n",
                    "value of y = \\\"some y value\\\"\\n",
                    "log point in testfunc\\n",
                    "log point in testfunc_with_param, param = \\\"test\\\"\\n"
                });
            });

            System.Threading.Thread.Sleep(1000);

            Console.WriteLine("Hello World!");      Label.Breakpoint("lp1");

            int x = 42;

            Console.WriteLine("x");                 Label.Breakpoint("lp2");

            string y = "some y value";

            Console.WriteLine("y");                 Label.Breakpoint("lp3");

            testfunc();

            testfunc_with_param("test");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
