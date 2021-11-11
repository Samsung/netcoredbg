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

        public string GetAndCheckValue(string caller_trace, string ExpectedResult1, string ExpectedResult2, string ExpectedType, string Expression)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", Expression));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(Expression, ((MIConst)res["exp"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedType, ((MIConst)res["type"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.True(ExpectedResult1 == ((MIConst)res["value"]).CString ||
                        ExpectedResult2 == ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

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
        static int Calc1()
        {
            return 15;
        }

        int Calc2()
        {
            return 16;
        }

        void TestVoidReturn()
        {
            Console.WriteLine("test void return");
        }

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

                // Test method calls.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "15", "int", "Calc1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "15", "int", "test_this_t.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this.Calc1()", "Error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "16", "int", "Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_this_t.Calc2()", "Error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "16", "int", "this.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"MITestEvaluate.test_this_t\\\"", "string", "ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"MITestEvaluate.test_this_t\\\"", "string", "this.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "Expression has been evaluated and has no value", "void", "TestVoidReturn()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "TestVoidReturn() + 1", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 + TestVoidReturn()", "error CS0019");

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

        public virtual int GetNumber()
        {
            return 10;
        }
    }
    public class test_child : test_parent
    {
        public int i_child = 402;

        public override int GetNumber()
        {
            return 11;
        }
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

    public class MethodCallTest1
    {
        public static int Calc1()
        {
            return 5;
        }

        public int Calc2()
        {
            return 6;
        }
    }

    public class MethodCallTest2
    {
        public static MethodCallTest1 member1 = new MethodCallTest1();
    }

    public class MethodCallTest3
    {
        public int Calc1(int i)
        {
            return i + 1;
        }

        public decimal Calc1(decimal d)
        {
            return d + 1;
        }

        public float Calc2(float f1, float f2, float f3)
        {
            return f1 * 100 + f2 * 10 + f3;
        }

        public int Calc2(int i1, int i2, int i3)
        {
            return i1 * 100 + i2 * 10 + i3;
        }

        public int Calc2(int i1, int i2, float f3)
        {
            return i1 * 100 + i2 * 10 + (int)f3;
        }
    }

    enum enumUnary_t
    {
        ONE,
        TWO
    };

    public class TestOperators1
    {
        public int data;
        public TestOperators1(int data_)
        {
            data = data_;
        }

        public static implicit operator TestOperators1(int value) => new TestOperators1(value);

        public static int operator +(TestOperators1 d1, int d2) => 55;
        public static int operator +(int d1, TestOperators1 d2) => 66;

        public static int operator ~(TestOperators1 d1) => ~d1.data;
        public static bool operator !(TestOperators1 d1) => true;
        public static int operator +(TestOperators1 d1, TestOperators1 d2) => d1.data + d2.data;
        public static int operator -(TestOperators1 d1, TestOperators1 d2) => d1.data - d2.data;
        public static int operator *(TestOperators1 d1, TestOperators1 d2) => d1.data * d2.data;
        public static int operator /(TestOperators1 d1, TestOperators1 d2) => d1.data / d2.data;
        public static int operator %(TestOperators1 d1, TestOperators1 d2) => d1.data % d2.data;
        public static int operator ^(TestOperators1 d1, TestOperators1 d2) => d1.data ^ d2.data;
        public static int operator &(TestOperators1 d1, TestOperators1 d2) => d1.data & d2.data;
        public static int operator |(TestOperators1 d1, TestOperators1 d2) => d1.data | d2.data;
        public static int operator >>(TestOperators1 d1, int d2) => d1.data >> d2;
        public static int operator <<(TestOperators1 d1, int d2) => d1.data << d2;
        public static bool operator ==(TestOperators1 d1, TestOperators1 d2) => d1.data == d2.data;
        public static bool operator !=(TestOperators1 d1, TestOperators1 d2) => d1.data != d2.data;
        public static bool operator <(TestOperators1 d1, TestOperators1 d2) => d1.data < d2.data;
        public static bool operator <=(TestOperators1 d1, TestOperators1 d2) => d1.data <= d2.data;
        public static bool operator >(TestOperators1 d1, TestOperators1 d2) => d1.data > d2.data;
        public static bool operator >=(TestOperators1 d1, TestOperators1 d2) => d1.data >= d2.data;
    }

    public struct TestOperators2
    {
        public int data;
        public TestOperators2(int data_)
        {
            data = data_;
        }
        public static implicit operator TestOperators2(int value) => new TestOperators2(value);

        public static int operator +(TestOperators2 d1, int d2) => 55;
        public static int operator +(int d1, TestOperators2 d2) => 66;

        public static int operator ~(TestOperators2 d1) => ~d1.data;
        public static bool operator !(TestOperators2 d1) => true;
        public static int operator +(TestOperators2 d1, TestOperators2 d2) => d1.data + d2.data;
        public static int operator -(TestOperators2 d1, TestOperators2 d2) => d1.data - d2.data;
        public static int operator *(TestOperators2 d1, TestOperators2 d2) => d1.data * d2.data;
        public static int operator /(TestOperators2 d1, TestOperators2 d2) => d1.data / d2.data;
        public static int operator %(TestOperators2 d1, TestOperators2 d2) => d1.data % d2.data;
        public static int operator ^(TestOperators2 d1, TestOperators2 d2) => d1.data ^ d2.data;
        public static int operator &(TestOperators2 d1, TestOperators2 d2) => d1.data & d2.data;
        public static int operator |(TestOperators2 d1, TestOperators2 d2) => d1.data | d2.data;
        public static int operator >>(TestOperators2 d1, int d2) => d1.data >> d2;
        public static int operator <<(TestOperators2 d1, int d2) => d1.data << d2;
        public static bool operator ==(TestOperators2 d1, TestOperators2 d2) => d1.data == d2.data;
        public static bool operator !=(TestOperators2 d1, TestOperators2 d2) => d1.data != d2.data;
        public static bool operator <(TestOperators2 d1, TestOperators2 d2) => d1.data < d2.data;
        public static bool operator <=(TestOperators2 d1, TestOperators2 d2) => d1.data <= d2.data;
        public static bool operator >(TestOperators2 d1, TestOperators2 d2) => d1.data > d2.data;
        public static bool operator >=(TestOperators2 d1, TestOperators2 d2) => d1.data >= d2.data;
    }

    public struct TestOperators3
    {
        public int data;
        public TestOperators3(int data_)
        {
            data = data_;
        }

        // Note, in order to test that was used proper operator, we use fixed return here.

        public static implicit operator int(TestOperators3 value) => 777;

        public static int operator +(TestOperators3 d1, int d2) => 555;
        public static int operator +(int d1, TestOperators3 d2) => 666;
        public static int operator >>(TestOperators3 d1, int d2) => 777;
        public static int operator <<(TestOperators3 d1, int d2) => 888;
    }

    class Program
    {
        int int_i = 505;
        static test_nested test_nested_static_instance;

        static int stGetInt()
        {
            return 111;
        }

        static int stGetInt(int x)
        {
            return x * 2;
        }

        int getInt()
        {
            return 222;
        }

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
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK11");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK12");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK13");
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

            // Test expression calculation.
            TestOperators1 testClass = new TestOperators1(12);
            TestOperators2 testStruct;
            TestOperators3 testStruct2;
            testStruct.data = 5;
            string testString1 = null;
            string testString2 = "test";
            int break_line2 = 1;                                                                    Label.Breakpoint("BREAK2");

            Label.Checkpoint("expression_test", "static_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "1 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "float", "1u + 1f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "long", "1u + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "decimal", "1m + 1m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "decimal", "1m + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "decimal", "1 + 1m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "66", "int", "1 + testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "testClass + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "66", "int", "1 + testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "testStruct + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "666", "int", "1 + testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "testStruct2 + 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"stringC\\\"", "string", "\\\"string\\\" + 'C'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test\\\"", "string", "testString1 + testString2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test\\\"", "string", "testString2 + testString1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"\\\"", "string", "testString1 + testString1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"testtest\\\"", "string", "testString2 + testString2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test\\\"", "string", "\\\"\\\" + \\\"test\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"test\\\"", "string", "\\\"test\\\" + \\\"\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"\\\"", "string", "\\\"\\\" + \\\"\\\"");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"testtest\\\"", "string", "\\\"test\\\" + \\\"test\\\"");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1UL + 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true + 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 + not_var", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1u + testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testClass + 1u", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1u + testStruct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct + 1u", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testClass + testStruct", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct + testClass", "error CS0019");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "3 - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "decimal", "3m - 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true - 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 - not_var", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-11", "int", "1 - testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "testClass - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-4", "int", "1 - testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "testStruct - 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-776", "int", "1 - testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "776", "int", "testStruct2 - 1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "2 * 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "decimal", "2m * 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true * 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 * not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "2 / 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "decimal", "2m / 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true / 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 / not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "2 % 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "decimal", "2m % 2m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true % 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 % not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "1 << 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m << 2m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true << 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 << not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "4 >> 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m >> 2m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true >> 2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 >> not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-2", "int", "~1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-13", "int", "~testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-6", "int", "~testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "~1m", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "~true", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "~not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true && true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "true && false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false && true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false && false");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 && 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m && 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true && not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true || true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true || false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "false || true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false || false");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 || 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m || 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true || not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "true ^ true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true ^ false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "false ^ true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false ^ false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "1 ^ 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "1 ^ 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "0 ^ 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "0 ^ 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "14", "int", "testClass ^ 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "14", "int", "2 ^ testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "testClass ^ testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "testStruct ^ 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "2 ^ testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "testStruct ^ testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct2 ^ testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m ^ 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 ^ not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true & true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "true & false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false & true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false & false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "3 & 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "5 & 8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "10 & 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "0 & 7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "0 & 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "testClass & 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "2 & testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "12", "int", "testClass & testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "testStruct & 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "2 & testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "testStruct & testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct2 & testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m & 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 & not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true | true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "true | false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "false | true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "false | false");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "13", "int", "5 | 8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "10 | 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "0 | 7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "0 | 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "14", "int", "testClass | 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "14", "int", "2 | testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "12", "int", "testClass | testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "testStruct | 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "2 | testStruct");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "testStruct | testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct2 | testStruct2", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1m | 1m", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 | not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "!true");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "!testClass");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "!testStruct");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "!1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "!1m", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "!not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "1 == 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "2 == 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "2m == 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 == not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1 != 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "2 != 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "2m != 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 != not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "1 < 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1 < 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1m < 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true < false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 < not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "2 > 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1 > 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1m > 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true > false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 > not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "1 <= 2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "1 <= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1 <= 0");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "1m <= 0m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true <= false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 <= not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "2 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "true", "bool", "1 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "0 >= 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "false", "bool", "0m >= 1m");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "true >= false", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1 >= not_var", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "2 << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "888", "int", "testStruct2 << testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "24", "int", "testClass << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "testStruct << 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "20", "int", "testStruct << 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1f << 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1f << 1f", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testClass << testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct << testStruct", "error CS0019");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "4 >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "777", "int", "testStruct2 >> testStruct2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "testClass >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "testStruct >> 1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "testStruct >> 2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1f >> 1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "1f >> 1f", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testClass >> testClass", "error CS0019");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "testStruct >> testStruct", "error CS0019");

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
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "Program.test_nested_static_instance.nested_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "MITestEvaluate.Program.test_nested_static_instance.nested_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.test_static_child", "error:");

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

            Label.Checkpoint("conditional_access_test", "method_calls_test", (Object context) => {
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

            // Test method calls.

            int Calc3()
            {
                return 8;
            }

            MethodCallTest1 MethodCallTest = new MethodCallTest1();
            test_child TestCallChild = new test_child();
            test_parent TestCallParentOverride = new test_child();
            test_parent TestCallParent = new test_parent();

            decimal decimalToString = 1.01M;
            double doubleToString = 2.02;
            float floatToString = 3.03f;
            char charToString = 'c';
            bool boolToString = true;
            sbyte sbyteToString = -5;
            byte byteToString = 5;
            short shortToString = -6;
            ushort ushortToString = 6;
            int intToString = -7;
            uint uintToString = 7;
            long longToString = -8;
            ulong ulongToString = 8;
            string stringToString = "string";

            MethodCallTest3 mcTest3 = new MethodCallTest3();

            int break_line11 = 1;                                                                        Label.Breakpoint("BREAK11");

            Label.Checkpoint("method_calls_test", "unary_operators_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK11");

                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MethodCallTest.Calc1()", "Error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "MethodCallTest.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MethodCallTest?.Calc1()", "Error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "MethodCallTest?.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"MITestEvaluate.MethodCallTest1\\\"", "string", "MethodCallTest?.ToString()");

                // Call non static method in static member.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "MITestEvaluate.MethodCallTest2.member1.Calc2()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "MethodCallTest2.member1.Calc2()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.MethodCallTest2.member1.Calc1()", "Error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MethodCallTest2.member1.Calc1()", "Error");

                // Call static method.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "MITestEvaluate.MethodCallTest1.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MITestEvaluate.MethodCallTest1.Calc2()", "Error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "MethodCallTest1.Calc1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "MethodCallTest1.Calc2()", "Error");

                // Call virtual/override.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "TestCallChild.GetNumber()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "TestCallParentOverride.GetNumber()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "TestCallParent.GetNumber()");

                // Call built-in types methods.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"1.01\\\"", "\\\"1,01\\\"", "string", "1.01M.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"1.01\\\"", "\\\"1,01\\\"", "string", "decimalToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"2.02\\\"", "\\\"2,02\\\"", "string", "2.02.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"2.02\\\"", "\\\"2,02\\\"", "string", "doubleToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"3.03\\\"", "\\\"3,03\\\"", "string", "3.03f.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"3.03\\\"", "\\\"3,03\\\"", "string", "floatToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"c\\\"", "string", "'c'.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"c\\\"", "string", "charToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"True\\\"", "string", "boolToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"-5\\\"", "string", "sbyteToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"5\\\"", "string", "byteToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"-6\\\"", "string", "shortToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"6\\\"", "string", "ushortToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"6\\\"", "string", "MethodCallTest?.Calc2().ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"7\\\"", "string", "7.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"-7\\\"", "string", "intToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"7\\\"", "string", "uintToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"-8\\\"", "string", "longToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"8\\\"", "string", "ulongToString.ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"string\\\"", "string", "\\\"string\\\".ToString()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"string\\\"", "string", "stringToString.ToString()");

                // Call with arguments.
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2.0", "decimal", "mcTest3.Calc1(1.0M)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "123", "int", "mcTest3.Calc2(1, 2, 3)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "456", "int", "mcTest3.Calc2(4, 5, 6.0f)");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test unary plus and negation operators.

            char charUnary = '영';
            sbyte sbyteUnary = -5;
            byte byteUnary = 5;
            short shortUnary = -6;
            ushort ushortUnary = 6;
            int intUnary = -7;
            uint uintUnary = 7;
            long longUnary = -8;
            ulong ulongUnary = 8;

            decimal decimalUnary = 1.01M;
            double doubleUnary = 2.02;
            float floatUnary = 3.03f;

            enumUnary_t enumUnary = enumUnary_t.TWO;

            int break_line12 = 1;                                                                        Label.Breakpoint("BREAK12");

            Label.Checkpoint("unary_operators_test", "function_evaluation_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK12");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "TWO", "MITestEvaluate.enumUnary_t", "enumUnary");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "+enumUnary", "error CS0023");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "-enumUnary", "error CS0023");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "49", "int", "+'1'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-49", "int", "-'1'");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50689", "int", "+charUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-50689", "int", "-charUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-5", "int", "+sbyteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "-sbyteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "+byteUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-5", "int", "-byteUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-6", "int", "+shortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "-shortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "+ushortUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-6", "int", "-ushortUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "+7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-7", "int", "+intUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-7", "int", "-7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "-(-7)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-7", "int", "+(-7)");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "int", "-intUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "uint", "+7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "7", "uint", "+uintUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-7", "long", "-7u");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-7", "long", "-uintUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "8", "long", "+8L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-8", "long", "+longUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-8", "long", "-8L");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "8", "long", "-longUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "8", "ulong", "+8UL");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "8", "ulong", "+ulongUnary");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "-8UL", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "-ulongUnary", "error");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "decimal", "+10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1.01", "decimal", "+decimalUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-10.5", "decimal", "-10.5m");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-1.01", "decimal", "-decimalUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "double", "+10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2.02", "double", "+doubleUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-10.5", "double", "-10.5d");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-2.02", "double", "-doubleUnary");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10.5", "float", "+10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "3.03", "float", "+floatUnary");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-10.5", "float", "-10.5f");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "-3.03", "float", "-floatUnary");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int break_line_13 = 13;                                                                           Label.Breakpoint("BREAK13");
            Label.Checkpoint("function_evaluation_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK13");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "stGetInt()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "666", "int", "stGetInt(333)");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "getInt()", "Error:");

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
