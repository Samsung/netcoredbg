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

        public void CheckErrorAtRequest(string caller_trace, string Expression, string errMsgStart)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", Expression));
            Assert.Equal(MIResultClass.Error, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.True(((MIConst)res["msg"]).CString.StartsWith(errMsgStart), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetResultAsString(string caller_trace, string expr, out string strRes)
        {
            var res = MIDebugger.Request("-var-create - * \"" + expr + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n" + caller_trace);
            strRes = ((MIConst)res["value"]).CString;
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

namespace MITestSizeof
{
    public struct Point
    {
        public Point(byte tag, decimal x, decimal y) => (Tag, X, Y) = (tag, x, y);

        public decimal X { get; }
        public decimal Y { get; }
        public byte Tag { get; }
    }


    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "expression_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.Continue(@"__FILE__:__LINE__");
            });

            int a = 10;
            TestStruct tc = new TestStruct(a, 11);
            string str1 = "string1";
            uint c;
            unsafe { c = (uint)sizeof(Point); }
            uint d = c;                                        Label.Breakpoint("BREAK1");

            Label.Checkpoint("expression_test1", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(bool).ToString(), "sizeof(bool)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(byte).ToString(), "sizeof(byte)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(sbyte).ToString(), "sizeof(sbyte)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(char).ToString(), "sizeof(char)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(int).ToString(), "sizeof(int)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(uint).ToString(), "sizeof(uint)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(long).ToString(), "sizeof(long)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(ulong).ToString(), "sizeof(ulong)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(float).ToString(), "sizeof(float)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(double).ToString(), "sizeof(double)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(decimal).ToString(), "sizeof(decimal)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Boolean).ToString(), "sizeof(System.Boolean)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Byte).ToString(), "sizeof(System.Byte)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Char).ToString(), "sizeof(System.Char)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Decimal).ToString(), "sizeof(System.Decimal)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Double).ToString(), "sizeof(System.Double)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Int16).ToString(), "sizeof(System.Int16)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Int32).ToString(), "sizeof(System.Int32)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Int64).ToString(), "sizeof(System.Int64)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.SByte).ToString(), "sizeof(System.SByte)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.Single).ToString(), "sizeof(System.Single)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.UInt16).ToString(), "sizeof(System.UInt16)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.UInt32).ToString(), "sizeof(System.UInt32)");
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", sizeof(System.UInt64).ToString(), "sizeof(System.UInt64)");
                string ss1;
                Context.GetResultAsString(@"__FILE__:__LINE__", "sizeof(Point)", out ss1);
                Context.CalcAndCheckExpression(@"__FILE__:__LINE__", ss1, "c");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(a)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(tc)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(str1)", "error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(abcd)", "error: The type or namespace name");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(Program)", "Error:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "sizeof(tc.a)", "");

                Context.Continue(@"__FILE__:__LINE__");
            });

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
        }
    }
}
