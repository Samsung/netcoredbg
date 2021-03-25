using System;
using System.IO;

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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CalcAndCheckExpression(string caller_trace, string ExpectedResult, string expr)
        {
            var res = MIDebugger.Request("-var-create - * \"" + expr + "\"");

            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        MIDebugger MIDebugger;
    }
}

namespace MITestExpression
{
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "expression_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.Continue(@"__FILE__:__LINE__");
            });

            int a = 10;
            int b = 11;
            TestStruct tc = new TestStruct(a + 1, b);
            string str1 = "string1";
            string str2 = "string2";
            int c = tc.b + b;                                   Label.Breakpoint("BREAK1");

            Label.Checkpoint("expression_test1", "expression_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", "21", "a + b");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", "22", "tc.a + b");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", "\\\"string1string2\\\"", "str1 + str2");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int d = 99;
            int e = c + a;                                      Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test2", "expression_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", "109", "d + a");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine(str1 + str2);

            tc.IncA();

            Console.WriteLine("Hello World!");

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        struct TestStruct
        {
            public int a;
            public int b;

            public TestStruct(int x, int y)
            {
                a = x;
                b = y;
            }

            public void IncA()
            {
                a++;                                            Label.Breakpoint("BREAK3");

                Label.Checkpoint("expression_test3", "finish", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");

                    Context.CalcAndCheckExpression(@"__FILE__:__LINE__", "12", "a + 1");

                    Context.Continue(@"__FILE__:__LINE__");
                });
            }
        }
    }
}
