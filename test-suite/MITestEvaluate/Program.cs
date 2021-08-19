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

        public void GetAndCheckChildValue(string caller_trace, string ExpectedResult, string ExpectedType, string VariableName, string ChildName)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", VariableName));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var var_name = ((MIConst)res["name"]).CString;
            res = MIDebugger.Request("-var-list-children --simple-values " + "\"" + var_name + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var children = (MIList)res["children"];
            string child_var_name = "";
            for (int i = 0; i < ((MIConst)res["numchild"]).Int; i++)
            {
                var child = (MITuple)((MIResult)children[i]).Value;

                if (((MIConst)child["exp"]).CString == ChildName)
                {
                    Assert.Equal(ExpectedType, ((MIConst)child["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(ExpectedResult, ((MIConst)child["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }

                if (((MIConst)child["exp"]).CString == "Static members")
                {
                    child_var_name = ((MIConst)child["name"]).CString;
                }
            }
            Assert.True(child_var_name != "", @"__FILE__:__LINE__"+"\n"+caller_trace);

            res = MIDebugger.Request("-var-list-children --simple-values " + "\"" + child_var_name + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            children = (MIList)res["children"];
            for (int i = 0; i < ((MIConst)res["numchild"]).Int; i++)
            {
                var child = (MITuple)((MIResult)children[i]).Value;
                if (((MIConst)child["exp"]).CString == ChildName)
                {
                    Assert.Equal(ExpectedType, ((MIConst)child["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    Assert.Equal(ExpectedResult, ((MIConst)child["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
                    return;
                }
            }

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void AddExceptionBreakpoint(string caller_trace, string excStage, string excFilter)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-exception-insert " + excStage + " " + excFilter).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExceptionBreakpointHit(string caller_trace, string bpName, string excCategory, string excStage, string excName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];
                var category = (MIConst)output["exception-category"];
                var stage = (MIConst)output["exception-stage"];
                var name = (MIConst)output["exception-name"];

                if (reason.CString != "exception-received" ||
                    category.CString != excCategory ||
                    stage.CString != excStage ||
                    name.CString != excName) {
                    return false;
                }

                var frame = (MITuple)output["frame"];
                var fileName = (MIConst)(frame["file"]);
                var numLine = (MIConst)(frame["line"]);

                if (fileName.CString == bp.FileName &&
                    numLine.CString == bp.NumLine.ToString()) {
                    return true;
                }

                return false;
            };

            Assert.True(MIDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger { get; private set; }
    }
}

namespace MITestEvaluate
{
    public struct test_struct1_t
    {
        public test_struct1_t(int x)
        {
            field_i1 = x;
        }
        public int field_i1;
    }

    public class test_class1_t
    {
        public double field_d1 = 7.1;
    }

    public class test_static_class1_t
    {
        ~test_static_class1_t()
        {
            // must be never called in this test!
            throw new System.Exception("test_static_class1_t finalizer called!");
        }

        public int field_i1;
        public test_struct1_t st2 = new test_struct1_t(9);

        public static test_struct1_t st = new test_struct1_t(8);
        public static test_class1_t cl = new test_class1_t();

        public static int static_field_i1 = 5;
        public static int static_property_i2
        { get { return static_field_i1 + 2; }}
    }

    public struct test_static_struct1_t
    {
        public static int static_field_i1 = 4;
        public static float static_field_f1 = 3.0f;
        public int field_i1;

        public static int static_property_i2
        { get { return static_field_i1 + 2; }}
    }

    public class test_this_t
    {
        public int this_i = 1;
        public string this_static_str = "2str";
        public static int this_static_i = 3;

        public void func(int arg_test)
        {
            int this_i = 4;
            int break_line4 = 1;                                                            Label.Breakpoint("BREAK4");

            Label.Checkpoint("this_test", "nested_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK4");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "501", "int", "arg_test");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.test_this_t}", "MITestEvaluate.test_this_t", "this");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "1", "int", "this", "this_i");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "\\\"2str\\\"", "string", "this", "this_static_str");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "3", "int", "this", "this_static_i");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "this_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"2str\\\"", "string", "this_static_str");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "3", "int", "this_static_i");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_this_t.this_i", "error:"); // error, cannot be accessed in this way
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_this_t.this_static_str", "error:"); // error, cannot be accessed in this way
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "3", "int", "MITestEvaluate.test_this_t.this_static_i");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }

    public class test_nested
    {
        public static int nested_static_i = 53;
        public int nested_i = 55;

        public class test_nested_1
        {
           // class without members

            public class test_nested_2
            {
                public static int nested_static_i = 253;
                public int nested_i = 255;

                public void func()
                {
                    int break_line5 = 1;                                                            Label.Breakpoint("BREAK5");

                    Label.Checkpoint("nested_test", "base_class_test", (Object context) => {
                        Context Context = (Context)context;
                        Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK5");

                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "MITestEvaluate.test_static_class1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "MITestEvaluate.test_static_struct1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "53", "int", "MITestEvaluate.test_nested.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "253", "int", "MITestEvaluate.test_nested.test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "353", "int", "MITestEvaluate.test_nested.test_nested_1.test_nested_3.nested_static_i");

                        // nested tests:
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "253", "int", "nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "test_static_class1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "test_static_struct1_t.static_field_i1");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "53", "int", "test_nested.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "253", "int", "test_nested.test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "253", "int", "test_nested_1.test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "253", "int", "test_nested_2.nested_static_i");
                        Context.GetAndCheckValue(@"__FILE__:__LINE__", "353", "int", "test_nested_3.nested_static_i");
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_nested.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_nested.test_nested_1.test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_nested_1.test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_nested_2.nested_i", "error:"); // error, cannot be accessed
                        Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_nested_3.nested_i", "error:"); // error, cannot be accessed

                        Context.Continue(@"__FILE__:__LINE__");
                    });
                }
            }
            public class test_nested_3
            {
                public static int nested_static_i = 353;
            }
        }
    }

    abstract public class test_static_parent
    {
        static public int static_i_f_parent = 301;
        static public int static_i_p_parent
        { get { return 302; }}
        public abstract void test();
    }
    public class test_static_child : test_static_parent
    {
        static public int static_i_f_child = 401;
        static public int static_i_p_child
        { get { return 402; }}

        public override void test()
        {
            int break_line5 = 1;                                                            Label.Breakpoint("BREAK7");
        }
    }

    public class test_parent
    {
        public int i_parent = 302;
    }
    public class test_child : test_parent
    {
        public int i_child = 402;
    }

    public class conditional_access1
    {
        public int i = 555;
        public int index = 1;
    }

    public class conditional_access2
    {
        public conditional_access1 member = new conditional_access1();
    }

    public class conditional_access3
    {
        public conditional_access1 member;
    }

    public class object_with_array
    {
        public int[] array = new int[] { 551, 552, 553 };
    }

    public struct test_array
    {
        public int i;
    }

    delegate void Lambda(string argVar);

    class Program
    {
        int int_i = 505;
        static test_nested test_nested_static_instance;

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK5");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK6");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK7");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK8");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK9");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK10");
                Context.Continue(@"__FILE__:__LINE__");
            });

            decimal dec = 12345678901234567890123456m;
            decimal long_zero_dec = 0.00000000000000000017M;
            decimal short_zero_dec = 0.17M;
            int[] array1 = new int[] { 10, 20, 30, 40, 50 };
            int[,] multi_array2 = { { 101, 102, 103 }, { 104, 105, 106 } };
            test_array[] array2 = new test_array[4];
            array2[0] = new test_array();
            array2[0].i = 201;
            array2[2] = new test_array();
            array2[2].i = 401;
            int i1 = 0;
            int i2 = 2;
            int i3 = 4;
            int i4 = 1;
            int break_line1 = 1;                                                                    Label.Breakpoint("BREAK1");

            Label.Checkpoint("values_test", "expression_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                string varName = Context.GetAndCheckValue(@"__FILE__:__LINE__", "12345678901234567890123456", "decimal", "dec");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0.00000000000000000017", "decimal", "long_zero_dec");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0.17", "decimal", "short_zero_dec");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{int[5]}", "int[]", "array1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "array1[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "30", "int", "array1[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "array1[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "array1[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "30", "int", "array1[i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "array1[i3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "array1[ 0]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "30", "int", "array1[2 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "array1 [ 4 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{int[2, 3]}", "int[,]", "multi_array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "101", "int", "multi_array2[0,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "105", "int", "multi_array2[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "104", "int", "multi_array2[1,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "102", "int", "multi_array2[0,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "104", "int", "multi_array2[i4,i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "102", "int", "multi_array2[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "101", "int", "multi_array2[ 0 , 0 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "105", "int", "multi_array2  [ 1,1 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.test_array[4]}", "MITestEvaluate.test_array[]", "array2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.test_array}", "MITestEvaluate.test_array", "array2[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "201", "int", "array2[i1].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "201", "int", "array2[0].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "201", "int", "array2   [   0   ]   .   i"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.test_array}", "MITestEvaluate.test_array", "array2[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "401", "int", "array2[i2].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "401", "int", "array2[2].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "401", "int", "array2  [  2  ]  .  i"); // check spaces

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this.int_i", "error:"); // error, Main is static method that don't have "this"
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "int_i", "error:"); // error, don't have "this" (no object of type "Program" was created)
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "not_declared", "error:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "array1[]", "error CS1519:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "multi_array2[]", "error CS1519:"); // error, no such variable
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "multi_array2[,]", "error CS1519:"); // error, no such variable

                var attrDResult = Context.MIDebugger.Request("9-var-show-attributes " + varName);
                Assert.Equal(MIResultClass.Done, attrDResult.Class, @"__FILE__:__LINE__");
                Assert.Equal("editable", ((MIConst)attrDResult["status"]).CString, @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("-var-delete " + varName).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request("-var-show-attributes " + varName).Class,
                             @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });


            int int_i1 = 5;
            int int_i2 = 5;
            string str_s1 = "one";
            string str_s2 = "two";
            int break_line2 = 1;                                                                    Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test", "static_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "int_i1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "int_i1 + int_i2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"onetwo\\\"", "", "\\\"one\\\" + \\\"two\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"onetwo\\\"", "", "str_s1 + \\\"two\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"onetwo\\\"", "", "str_s1 + str_s2");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "int_i1 +/ int_i2", "error CS1525:");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 + not_var", "System.AggregateException"); // error

                Context.Continue(@"__FILE__:__LINE__");
            });

            // switch to separate scope, in case `cl` constructor called by some reason, GC will able to care
            test_SuppressFinalize();
            void test_SuppressFinalize()
            {
                test_static_class1_t cl;
                test_static_struct1_t st;
                st.field_i1 = 2;
                int break_line3 = 1;                                                                Label.Breakpoint("BREAK3");
            }
            // in this way we check that finalizer was not called by GC for `cl`
            GC.Collect();

            Label.Checkpoint("static_test", "this_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");

                // test class fields/properties via local variable
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "5", "int", "cl", "static_field_i1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "7", "int", "cl", "static_property_i2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "cl.st", "error:"); // error CS0176: Member 'test_static_class1_t.st' cannot be accessed with an instance reference; qualify it with a type name instead
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "cl.cl", "error:"); // error CS0176: Member 'test_static_class1_t.cl' cannot be accessed with an instance reference; qualify it with a type name instead
                // test struct fields via local variable
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "4", "int", "st", "static_field_i1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "3", "float", "st", "static_field_f1");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "6", "int", "st", "static_property_i2");
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "2", "int", "st", "field_i1");
                // test direct eval for class and struct static fields/properties
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "MITestEvaluate.test_static_class1_t.static_field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "MITestEvaluate . test_static_class1_t  .  static_field_i1  "); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "MITestEvaluate.test_static_class1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "test_static_class1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "MITestEvaluate.test_static_struct1_t.static_field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "3", "float", "MITestEvaluate.test_static_struct1_t.static_field_f1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "MITestEvaluate.test_static_struct1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "test_static_struct1_t.static_property_i2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "8", "int", "MITestEvaluate.test_static_class1_t.st.field_i1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7.1", "double", "MITestEvaluate.test_static_class1_t.cl.field_d1");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_static_class1_t.not_declared", "error:"); // error, no such field in class
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_static_struct1_t.not_declared", "error:"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_static_class1_t.st.not_declared", "error:"); // error, no such field in struct
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_static_class1_t.cl.not_declared", "error:"); // error, no such field in class

                Context.Continue(@"__FILE__:__LINE__");
            });

            test_this_t test_this = new test_this_t();
            test_this.func(501);

            test_nested.test_nested_1.test_nested_2 test_nested = new test_nested.test_nested_1.test_nested_2();
            test_nested.func();

            test_nested_static_instance = new test_nested();

            test_child child = new test_child();

            int break_line6 = 1;                                                                     Label.Breakpoint("BREAK6");

            Label.Checkpoint("base_class_test", "override_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK6");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "402", "int", "child.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "302", "int", "child.i_parent");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "401", "int", "MITestEvaluate.test_static_child.static_i_f_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "402", "int", "MITestEvaluate.test_static_child.static_i_p_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "301", "int", "MITestEvaluate.test_static_child.static_i_f_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "302", "int", "MITestEvaluate.test_static_child.static_i_p_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.test_static_child}", "MITestEvaluate.test_static_child", "MITestEvaluate.test_static_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "Program.test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "MITestEvaluate.Program.test_nested_static_instance.nested_i");

                Context.Continue(@"__FILE__:__LINE__");
            });

            test_static_parent base_child = new test_static_child();
            base_child.test();

            Label.Checkpoint("override_test", "lambda_test1", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK7");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "401", "int", "static_i_f_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "402", "int", "static_i_p_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "301", "int", "static_i_f_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "302", "int", "static_i_p_parent");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test lambda.

            string mainVar = "mainVar";

            Lambda lambda1 = (argVar) => {
                string localVar = "localVar";

                Label.Checkpoint("lambda_test1", "lambda_test2", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK8");

                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"localVar\\\"", "string", "localVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                Console.WriteLine(mainVar);                                                             Label.Breakpoint("BREAK8");
            };

            lambda1("argVar");

            Lambda lambda2 = (argVar) => {
                string localVar = "localVar";

                Label.Checkpoint("lambda_test2", "internal_var_test", (Object context) => {
                    Context Context = (Context)context;
                    Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK9");

                    Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar", "error:");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"localVar\\\"", "string", "localVar");
                    Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");

                    Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");

                    Context.Continue(@"__FILE__:__LINE__");
                });

                int break_line6 = 1;                                                                     Label.Breakpoint("BREAK9");
            };

            lambda2("argVar");

            // Test internal "$exception" variable name.

            try
            {
                throw new System.Exception();                                                            Label.Breakpoint("BP_EXCEPTION");
            }
            catch {}

            Label.Checkpoint("internal_var_test", "literals_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "BP_EXCEPTION", "clr", "throw", "System.Exception");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Exception}", "System.Exception", "$exception");
            });

            // Test literals.

            Label.Checkpoint("literals_test", "conditional_access_test", (Object context) => {
                Context Context = (Context)context;

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test\\\"", "string", "\\\"test\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"$exception\\\"", "string", "\\\"$exception\\\"");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "99 'c'", "char", "'c'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "decimal", "10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "double", "10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "float", "10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "15", "int", "0x0F");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "42", "int", "0b00101010");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "uint", "7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "long", "7L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "ulong", "7UL");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "object", "null");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "0b_0010_1010", "error"); // Error could be CS1013 or CS8107 here.
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "'𐌞'", "error CS1012"); // '𐌞' character need 2 whcars and not supported

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test conditional access.

            test_child child_null;
            conditional_access2 conditional_access_null;
            conditional_access2 conditional_access = new conditional_access2();
            conditional_access3 conditional_access_double_null = new conditional_access3();
            conditional_access3 conditional_access_double = new conditional_access3();
            conditional_access_double.member = new conditional_access1();
            int[] array1_null;
            conditional_access2[] conditional_array_null;
            conditional_access2[] conditional_array = new conditional_access2[] { new conditional_access2(), new conditional_access2() };
            object_with_array object_with_array_null;
            object_with_array object_with_array = new object_with_array();
            int break_line10 = 1;                                                                        Label.Breakpoint("BREAK10");

            Label.Checkpoint("conditional_access_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK10");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.test_child", "child_null?.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.test_child", "child_null?.i_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "402", "int", "child?.i_child");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "302", "int", "child?.i_parent");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.conditional_access1}", "MITestEvaluate.conditional_access1", "conditional_access?.member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "conditional_access?.member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access2", "conditional_access_null?.member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access2", "conditional_access_null?.member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access1", "conditional_access_double_null?.member?.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "conditional_access_double?.member?.i");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "array1?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "20", "int", "array1?[conditional_access?.member.index]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "int[]", "array1_null?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access2[]", "conditional_array_null?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access2[]", "conditional_array_null?[0].member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.conditional_access2[]", "conditional_array_null?[0].member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.conditional_access2}", "MITestEvaluate.conditional_access2", "conditional_array?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvaluate.conditional_access1}", "MITestEvaluate.conditional_access1", "conditional_array?[0].member");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "conditional_array?[0].member.i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestEvaluate.object_with_array", "object_with_array_null?.array[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "552", "int", "object_with_array?.array[1]");

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
