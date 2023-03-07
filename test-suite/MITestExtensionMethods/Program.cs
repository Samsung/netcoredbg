using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;

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

        public string GetAndCheckValue(string caller_trace, string ExpectedResult, string ExpectedType, string Expression)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", Expression));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(Expression, ((MIConst)res["exp"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedType, ((MIConst)res["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            return ((MIConst)res["name"]).CString;
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


namespace CustomExtensions
{
    public static class StringExtension
    {
        public static int WordCount(this string str)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length;
        }

        public static int WordCount(this string str, int i)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length + i;
        }

        public static int WordCount(this string str, int i, int j)
        {
            return str.Split(new char[] {' ', '.', '?'}, StringSplitOptions.RemoveEmptyEntries).Length + i + j;
        }
    }
}

namespace MITestExtensionMethods
{
    using CustomExtensions;

    public class MyString
    {
        string s;
        public MyString(string ms)
        {
            s = ms;
        }
    }

    struct MyInt
    {
        int i;
        public MyInt(int mi)
        {
            i = mi;
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            string s = "The quick brown fox jumped over the lazy dog.";
            List<string> lists = new List<string>();
            Label.Checkpoint("init", "expression_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.Continue(@"__FILE__:__LINE__");
            });

            lists.Add("null");
            lists.Add("first");
            lists.Add("second");
            lists.Add("third");
            lists.Add("fourth");
            string res = lists.ElementAt(1);                                    Label.Breakpoint("BREAK1");

            Label.Checkpoint("expression_test1", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "9", "int", "s.WordCount()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "s.WordCount(1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount(1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount( 1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount(1+1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount( 1+1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount( 1 + 1 )");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount(1,1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "13", "int", "s.WordCount(1+1,1+1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount(1,1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "s.WordCount(1,1)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "s.WordCount(1,1,1)", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "s.WordCount(\\\"first\\\")", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "s.WordCount(1, \\\"first\\\")", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "s.WordCount(\\\"first\\\", 1)", "Error: 0x80070057");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"null\\\"", "string", "lists.ElementAt(0)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"first\\\"", "string", "lists.ElementAt(1)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"second\\\"", "string", "lists.ElementAt(2)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"third\\\"", "string", "lists.ElementAt(3)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"fourth\\\"", "string", "lists.ElementAt(4)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists.ElemetAt()", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists.ElementAt(1,2)", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists.ElementAt(\\\"first\\\")", "Error: 0x80070057");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
