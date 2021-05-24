using System;
using System.IO;
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
            // Explicitly disable JMC for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set just-my-code 0").Class,
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

        public void WasBreakpointHit(string caller_trace, string bpName)
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
                var fileName = (MIConst)frame["file"];
                var line = ((MIConst)frame["line"]).Int;

                if (fileName.CString == bp.FileName &&
                    line == bp.NumLine) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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


namespace MITestNoJMCBreakpoint
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_attr1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_ATTR5");

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
            Label.Checkpoint("test_attr1", "test_attr2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK_ATTR1");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        [DebuggerNonUserCodeAttribute()]
        static void test_attr_func2()
        {                                                           Label.Breakpoint("BREAK_ATTR2");
            Label.Checkpoint("test_attr2", "test_attr4", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK_ATTR2");
                Context.Continue(@"__FILE__:__LINE__");
            });
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
            Label.Checkpoint("test_attr4", "test_attr5", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK_ATTR4");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class ctest_attr2
    {
        public static void test_func()
        {                                                           Label.Breakpoint("BREAK_ATTR5");
            Label.Checkpoint("test_attr5", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK_ATTR5");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
