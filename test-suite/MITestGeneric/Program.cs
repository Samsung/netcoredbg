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

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

            Assert.True(MIDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void Continue(string caller_trace)
        {
            Assert.Equal(MIResultClass.Running,
                         MIDebugger.Request("-exec-continue").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void CreateAndAssignVar(string caller_trace, string variable, string val, bool ignoreCheck = false)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", variable));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal("editable", ((MIConst)res["attributes"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string internalName = ((MIConst)res["name"]).CString;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request(String.Format("-var-assign {0} \"{1}\"", internalName, val)).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

            if (ignoreCheck)
                return;

            // This could be expression, get real value.
            res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", val));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var expectedVal = ((MIConst)res["value"]).CString;
            if (((MIConst)res["type"]).CString == "char")
            {
                int foundStr = expectedVal.IndexOf(" ");
                if (foundStr >= 0)
                    expectedVal = expectedVal.Remove(foundStr);
            }
            // Check that variable have assigned value.
            CreateAndCompareVar(@"__FILE__:__LINE__"+"\n"+caller_trace, variable, expectedVal);
        }

        public void CreateAndCompareVar(string caller_trace, string variable, string val)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", variable));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var curValue = ((MIConst)res["value"]).CString;
            var curType = ((MIConst)res["type"]).CString;
            if (curType == "char")
            {
                int foundStr = curValue.IndexOf(" ");
                if (foundStr >= 0)
                    curValue = curValue.Remove(foundStr);
            }
            Assert.Equal(val, curValue, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string varName = ((MIConst)res["name"]).CString;
            res = MIDebugger.Request(String.Format("-var-evaluate-expression {0}", varName));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            curValue = ((MIConst)res["value"]).CString;
            if (curType == "char")
            {
                int foundStr = curValue.IndexOf(" ");
                if (foundStr >= 0)
                    curValue = curValue.Remove(foundStr);
            }
            Assert.Equal(val, curValue, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

namespace MITestGeneric
{
    class Program
    {
        public class TestGeneric<T,U>
        {

            public T test1(T arg1)
            {
                return arg1;
            }

            public W test2<W>(W arg2)
            {
                return arg2;
            }

            static public T static_test1(T arg1)
            {
                return arg1;
            }

            static public W static_test2<W>(W arg2)
            {
                return arg2;
            }

            public T i1;
            public U s1;

            public T p_i1
            { get; set; }

            public U p_s1
            { get; set; }

            public static T static_i1;
            public static U static_s1;

            public static T static_p_i1
            { get; set; }

            public static U static_p_s1
            { get; set; }

            public void test_func()
            {
                Console.WriteLine("test_func()");                                                            Label.Breakpoint("BREAK2");

                Label.Checkpoint("test_func", "test_set_value", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestGeneric.Program.TestGeneric<int, string>}", "MITestGeneric.Program.TestGeneric<int, string>", "this");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "123", "int", "i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string1\\\"", "string", "s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "234", "int", "p_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string2\\\"", "string", "p_s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "345", "int", "static_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string3\\\"", "string", "static_s1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "456", "int", "static_p_i1");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string4\\\"", "string", "static_p_s1");

                    // FIXME
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "test1(5)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "test2<int>(10)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", "15", "int", "static_test1(15)");
                    //Context.GetAndCheckValue(@"__FILE__:__LINE__", "20", "int", "static_test2<int>(20)");

                    // Test set value.

                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "i1", "55");
                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "s1", "\"changed1\"", true);
                    Context.CreateAndCompareVar(@"__FILE__:__LINE__", "s1", "\\\"changed1\\\"");

                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "p_i1", "66");
                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "p_s1", "\"changed2\"", true);
                    Context.CreateAndCompareVar(@"__FILE__:__LINE__", "p_s1", "\\\"changed2\\\"");

                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "static_i1", "77");
                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "static_s1", "\"changed3\"", true);
                    Context.CreateAndCompareVar(@"__FILE__:__LINE__", "static_s1", "\\\"changed3\\\"");

                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "static_p_i1", "88");
                    Context.CreateAndAssignVar(@"__FILE__:__LINE__", "static_p_s1", "\"changed4\"", true);
                    Context.CreateAndCompareVar(@"__FILE__:__LINE__", "static_p_s1", "\\\"changed4\\\"");

                    Context.Continue(@"__FILE__:__LINE__");
                });
            }
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_main", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.Continue(@"__FILE__:__LINE__");
            });

            TestGeneric<int,string> ttt = new TestGeneric<int,string>();
            ttt.i1 = 123;
            ttt.s1 = "test_string1";
            ttt.p_i1 = 234;
            ttt.p_s1 = "test_string2";
            TestGeneric<int,string>.static_i1 = 345;
            TestGeneric<int,string>.static_s1 = "test_string3";
            TestGeneric<int,string>.static_p_i1 = 456;
            TestGeneric<int,string>.static_p_s1 = "test_string4";

            ttt.test_func();                                                                                 Label.Breakpoint("BREAK1");

            Label.Checkpoint("test_main", "test_func", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestGeneric.Program.TestGeneric<int, string>}", "MITestGeneric.Program.TestGeneric<int, string>", "ttt");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "123", "int", "ttt.i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string1\\\"", "string", "ttt.s1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "234", "int", "ttt.p_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string2\\\"", "string", "ttt.p_s1");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "ttt.static_i1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "ttt.static_s1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "ttt.static_p_i1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "ttt.static_p_s1", "error");

                // FIXME debugger must be fixed first
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "345", "int", "TestGeneric<int, string>.static_i1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string3\\\"", "string", "TestGeneric<int, string>.static_s1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "456", "int", "TestGeneric<int, string>.static_p_i1");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test_string4\\\"", "string", "TestGeneric<int, string>.static_p_s1");

                // FIXME debugger must be fixed first
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "ttt.test1(5)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "ttt.test2<int>(10)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "15", "int", "TestGeneric<int,string>.static_test1(15)");
                //Context.GetAndCheckValue(@"__FILE__:__LINE__", "20", "int", "TestGeneric<int,string>.static_test2<int>(20)");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Console.WriteLine("test set value");                                                             Label.Breakpoint("BREAK3");

            Label.Checkpoint("test_set_value", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ttt.i1", "555");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ttt.s1", "\"changed_string1\"", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "ttt.s1", "\\\"changed_string1\\\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ttt.p_i1", "666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ttt.p_s1", "\"changed_string2\"", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "ttt.p_s1", "\\\"changed_string2\\\"");

                // FIXME debugger must be fixed first
                //Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_i1", "777");
                //Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_s1", "\"changed_string3\"", true);
                //Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_s1", "\\\"changed_string3\\\"");

                // FIXME debugger must be fixed first
                //Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_p_i1", "888");
                //Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_p_s1", "\"changed_string4\"", true);
                //Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestGeneric<int,string>.static_p_s1", "\\\"changed_string4\\\"");

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
