using System;
using System.IO;
using System.Diagnostics;
using System.Threading;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        // https://docs.microsoft.com/en-us/visualstudio/extensibility/debugger/reference/evalflags
        public enum enum_EVALFLAGS {
            EVAL_RETURNVALUE = 0x0002,
            EVAL_NOSIDEEFFECTS = 0x0004,
            EVAL_ALLOWBPS = 0x0008,
            EVAL_ALLOWERRORREPORT = 0x0010,
            EVAL_FUNCTION_AS_ADDRESS = 0x0040,
            EVAL_NOFUNCEVAL = 0x0080,
            EVAL_NOEVENTS = 0x1000
        }

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

        public void CheckAttributes(string caller_trace, string variable, string expectedAttributes)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", variable));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(expectedAttributes, ((MIConst)res["attributes"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void ErrorAtAssignVar(string caller_trace, string variable, string val)
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", variable));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string internalName = ((MIConst)res["name"]).CString;

            Assert.Equal(MIResultClass.Error,
                         MIDebugger.Request(String.Format("-var-assign {0} \"{1}\"", internalName, val)).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void GetAndCheckChildValue(string caller_trace, string ExpectedResult, string variable,
                                          int childIndex, bool setEvalFlags, enum_EVALFLAGS evalFlags, string expectedAttributes = "editable")
        {
            var res = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", variable) +
                                         (setEvalFlags ? (" --evalFlags " + (int)evalFlags) : "" ));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string struct2 = ((MIConst)res["name"]).CString;

            res = MIDebugger.Request("-var-list-children --simple-values " +
                                     "\"" + struct2 + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var children = (MIList)res["children"];
            var child = (MITuple)((MIResult)children[childIndex]).Value;
            Assert.Equal(expectedAttributes, ((MIConst)child["attributes"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Assert.Equal(ExpectedResult, ((MIConst)child["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string varName = ((MIConst)child["name"]).CString;
            res = MIDebugger.Request(String.Format("-var-evaluate-expression {0}", varName));
            if (ExpectedResult == "<error>")
            {
                Assert.Equal(MIResultClass.Error, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
                return;
            }
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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger;
    }
}

namespace MITestVariables
{
    public class TestImplicitCast1
    {
        public int data;
        public TestImplicitCast1(int data_)
        {
            data = data_;
        }

        public static implicit operator TestImplicitCast1(char value) => new TestImplicitCast1((int)value);
        public static implicit operator TestImplicitCast1(int value) => new TestImplicitCast1(value);
        public static implicit operator TestImplicitCast1(decimal value) => new TestImplicitCast1((int)value);
        public static implicit operator int(TestImplicitCast1 value) => value.data;
        public static implicit operator decimal(TestImplicitCast1 value) => (decimal)value.data;

        public override string ToString()
        {
            return data.ToString();
        }

        public decimal GetDecimal()
        {
            return 11.1M;
        }
    }

    public class TestImplicitCast2
    {
        private long data;
        public TestImplicitCast2(long data_)
        {
            data = data_;
        }

        public static implicit operator TestImplicitCast2(TestImplicitCast1 value) => new TestImplicitCast2(value.data * 10);

        public override string ToString()
        {
            return data.ToString();
        }
    }

    public struct TestImplicitCast3
    {
        private float data;

        public TestImplicitCast3(decimal data_)
        {
            data = (float)data_;
        }

        public static implicit operator TestImplicitCast3(decimal value) => new TestImplicitCast3(value);

        public override string ToString()
        {
            return data.ToString();
        }
    }

    public struct TestSetVarStruct
    {
        public static int static_field_i;
        public int field_i;

        public static int static_prop_i
        { get; set; }
        public int prop_i
        { get; set; }

        public static int static_prop_i_noset
        { get {return 5001;} }
        public int prop_i_noset
        { get {return 5002;} }
    }

    public struct TestStruct3
    {
        public int val1
        {
            get
            {
                return 777; 
            }
        }
    }

    public struct TestStruct4
    {
        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.RootHidden)]
        public int val1
        {
            get
            {
                return 666; 
            }
        }

        [System.Diagnostics.DebuggerBrowsable(DebuggerBrowsableState.Never)]
        public int val2
        {
            get
            {
                return 777; 
            }
        }

        public int val3
        {
            get
            {
                return 888; 
            }
        }
    }

    public struct TestStruct5
    {
        public int val1
        {
            get
            {
                return 111; 
            }
        }

        public int val2
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 222; 
            }
        }

        public string val3
        {
            get
            {
                return "text_333"; 
            }
        }

        public float val4
        {
            get
            {
                System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();
                return 444.4f; 
            }
        }

        public float val5
        {
            get
            {
                return 555.5f; 
            }
        }
    }

    public struct TestStruct6
    {
        public int val1
        {
            get
            {
                // Test, that debugger ignore Break() callback during eval.
                Debugger.Break();
                return 123; 
            }
        }

        public int val2
        {
            get
            {
                // CoreCLR 7.0 and 8.0 have issue with abortable internal native code.
                // https://github.com/dotnet/runtime/issues/82422
                if (System.Environment.Version.Major == 7 ||
                    System.Environment.Version.Major == 8)
                {
                    while (true)
                    {
                        System.Threading.Thread.Sleep(100);
                    }
                }
                else
                    System.Threading.Thread.Sleep(5000000);
                return 999; 
            }
        }

        public string val3
        {
            get
            {
                // Test, that debugger ignore Breakpoint() callback during eval.
                return "text_123";                              Label.Breakpoint("BREAK_GETTER");
            }
        }
    }

    public struct TestStruct7
    {
        public int val1
        {
            get
            {
                return 567; 
            }
        }

        public int val2
        {
            get
            {
                try {
                    throw new System.DivideByZeroException();
                }
                catch
                {
                    return 777; 
                }
                return 888; 
            }
        }

        public int val3
        {
            get
            {
                throw new System.DivideByZeroException();
                return 777; 
            }
        }

        public string val4
        {
            get
            {
                return "text_567"; 
            }
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "setup_var", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                // Test evaluation getter with exception (must not break with exception breakpoints).
                Context.MIDebugger.Request("-break-exception-insert throw+user-unhandled *");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK5");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK6");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK7");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK_GETTER");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_func1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_func2");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // Test set variable.

            sbyte   testSByte = -2;
            byte    testByte = 1;
            short   testShort = -3;
            ushort  testUShort = 4;
            int     testInt = -5;
            uint    testUInt = 6;
            long    testLong = -7;
            ulong   testULong = 8;
            float   testFloat = 9.9f;
            double  testDouble = 10.1;
            decimal testDecimal = 11.11M;
            char    testChar = 'ㅎ';
            bool    testBool = true;
            string  testString = "some string that I'll test with";
            TestImplicitCast1 testClass = new TestImplicitCast1(12);

            sbyte   varSByte = -102;
            byte    varByte = 101;
            short   varShort = -103;
            ushort  varUShort = 104;
            int     varInt = -105;
            uint    varUInt = 106;
            long    varLong = -107;
            ulong   varULong = 108;
            float   varFloat = 109.9f;
            double  varDouble = 1010.1;
            decimal varDecimal = 1011.11M;
            char    varChar = 'Ф';
            bool    varBool = false;
            string  varString = "another string";
            TestImplicitCast1 varClass = new TestImplicitCast1(112);
            TestImplicitCast2 varClass2 = new TestImplicitCast2(312);
            TestImplicitCast3 varStruct3;

            sbyte   litSByte = -103;
            byte    litByte = 102;
            short   litShort = -104;
            ushort  litUShort = 204;
            int     litInt = -205;
            uint    litUInt = 206;
            long    litLong = -207;
            ulong   litULong = 208;
            float   litFloat = 209.9f;
            double  litDouble = 2010.1;
            decimal litDecimal = 2011.11M;
            char    litChar = 'Й';
            bool    litBool = false;
            string  litString = "string";
            TestImplicitCast1 litClass = new TestImplicitCast1(212);

            int[] array1 = new int[] { 1, 2, 3, 4, 5 };

            TestSetVarStruct setVarStruct = new TestSetVarStruct();
            TestSetVarStruct.static_field_i = 1001;
            TestSetVarStruct.static_prop_i = 1002;
            setVarStruct.field_i = 2001;
            setVarStruct.prop_i = 2002;

            int dummy1 = 1;                                     Label.Breakpoint("BREAK1");

            Label.Checkpoint("setup_var", "test_var", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varChar", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testSByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varChar", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litChar", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310.1f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310.1d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "310.1m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litChar", "\"string\"");

                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varByte", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varByte", "testClass");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "301");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litByte", "103");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "-103");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "103u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "-103L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "103ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "103f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "103d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "103m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litByte", "\"string\"");

                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varSByte", "testSByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varSByte", "testClass");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "-301");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litSByte", "-105");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "103u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "-103L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "103ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "-103f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "-103d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "-103m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litSByte", "\"string\"");

                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varShort", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varShort", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varShort", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varShort", "testClass");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "-30000005");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litShort", "-205");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "205u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "-205L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "205ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "205f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "205d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "205m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litShort", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUShort", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUShort", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUShort", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUShort", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litUShort", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "30000005");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litUShort", "205");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "-205");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "205m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUShort", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varInt", "testString");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varInt", "testClass", true);
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litInt", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "-2147483649");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litInt", "-305");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "305u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "-305L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "305ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "-305f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "-305d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "-305m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litInt", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUInt", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUInt", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUInt", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varUInt", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varUInt", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litUInt", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "4294967297");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litUInt", "306");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "-306");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litUInt", "306u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "306L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "306ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "306f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "306d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "306m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litUInt", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testUInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varLong", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varLong", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litLong", "'A'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litLong", "-307");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litLong", "307u");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litLong", "-307L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "307ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "-307f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "-307d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "-307m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litLong", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varULong", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varULong", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varULong", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varULong", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testLong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varULong", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varULong", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litULong", "'A'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litULong", "308");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "-308");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litULong", "308u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "308L");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litULong", "308ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "308f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "308d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "308m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litULong", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testUInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testLong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testULong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varFloat", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varFloat", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varFloat", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varFloat", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varFloat", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varFloat", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "'A'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "309");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "309u");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "309L");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "309ul");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litFloat", "309.9f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litFloat", "309.9d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litFloat", "309.9m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litFloat", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litFloat", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testUInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testLong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testULong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testFloat", true);
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDouble", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDouble", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDouble", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDouble", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDouble", "testClass");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "'A'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310u");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310L");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310ul");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310.1f", true);
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDouble", "310.1d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDouble", "310.1m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDouble", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDouble", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testChar");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testSByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testByte");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testUInt");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testLong");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDecimal", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDecimal", "testDouble");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDecimal", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varDecimal", "testString");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varDecimal", "testClass", true);
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "'A'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "311");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "311u");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "311L");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "311ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDecimal", "311.11f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDecimal", "311.11d");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litDecimal", "311.11m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDecimal", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litDecimal", "\"string\"");

                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testSByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testDecimal");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varBool", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varBool", "testClass");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310.1f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310.1d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "310.1m");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litBool", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litBool", "\"string\"");

                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testChar");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testSByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testUShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testDouble");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testDecimal");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testBool");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varString", "testString");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varString", "testClass");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "'A'");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310.1f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310.1d");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "310.1m");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litString", "true");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litString", "\"string\"", true);

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass", "testChar", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"12622\\\"");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testSByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testByte");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testShort");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testUShort");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass", "testInt", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"-5\\\"");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testUInt");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testLong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testULong");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testFloat");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testDouble");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass", "testDecimal", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"11\\\"");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass", "varClass.GetDecimal()", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"11\\\"");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testBool");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "varClass", "testString");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass", "testClass");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"12\\\"");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litClass", "'A'", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litClass.ToString()", "\\\"65\\\"");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litClass", "5", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litClass.ToString()", "\\\"5\\\"");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "310u");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "310L");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "310ul");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "310.1f");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "310.1d");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "litClass", "310.1m", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litClass.ToString()", "\\\"310\\\"");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "true");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "litClass", "\"string\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varClass2", "testClass", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass2.ToString()", "\\\"120\\\"");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "varStruct3", "11m", true);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varStruct3.ToString()", "\\\"11\\\"");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[0]", "1");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[1]", "2");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[2]", "3");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[3]", "4");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[4]", "5");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "array1[1]", "11");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "array1[3]", "33");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_field_i", "1001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i", "1002");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i_noset", "5001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.field_i", "2001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.prop_i", "2002");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.prop_i_noset", "5002");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_field_i", "3001");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i", "3002");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i_noset", "3003");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "setVarStruct.field_i", "4001");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__",  "setVarStruct.prop_i", "4002");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "setVarStruct.prop_i_noset", "4003");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_field_i", "3001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i", "3002");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.field_i", "4001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.prop_i", "4002");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "1+1", "2");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "1", "1");
                Context.ErrorAtAssignVar(@"__FILE__:__LINE__", "1.ToString()", "\"1\"");

                Context.CheckAttributes(@"__FILE__:__LINE__", "array1[0]", "editable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "array1?[0]", "editable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "litClass.data", "editable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "litClass?.data", "editable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i", "editable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i_noset", "noneditable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "1", "noneditable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "-1", "noneditable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "-array1[0]", "noneditable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "1+1", "noneditable");
                Context.CheckAttributes(@"__FILE__:__LINE__", "litClass.data.ToString()", "noneditable");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                     Label.Breakpoint("BREAK2");

            Label.Checkpoint("test_var", "bp_func_test", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varChar", "12622");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litChar", "65");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varSByte", "-2");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litSByte", "-105");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varByte", "1");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litByte", "103");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varShort", "-3");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litShort", "-205");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varUShort", "4");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litUShort", "205");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varInt", "12");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litInt", "-305");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varUInt", "6");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litUInt", "306");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varLong", "-7");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litLong", "-307");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varULong", "8");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litULong", "308");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varFloat", "9.8999996");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litFloat", "309.89999");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varDouble", "10.1");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litDouble", "310.1");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varDecimal", "12");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litDecimal", "311.11");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varBool", "true");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litBool", "true");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varString", "\\\"some string that I'll test with\\\"");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litString", "\\\"string\\\"");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass.ToString()", "\\\"12\\\"");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "litClass.ToString()", "\\\"310\\\"");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varClass2.ToString()", "\\\"120\\\"");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "varStruct3.ToString()", "\\\"11\\\"");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[0]", "1");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[1]", "11");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[2]", "3");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[3]", "33");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "array1[4]", "5");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_field_i", "3001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "TestSetVarStruct.static_prop_i", "3002");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.field_i", "4001");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "setVarStruct.prop_i", "4002");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestFunctionArgs(10, 5f, "test_string");

            TestStruct3 ts3 = new TestStruct3();

            int dummy3 = 3;                                     Label.Breakpoint("BREAK3");

            Label.Checkpoint("test_eval_flags", "test_debugger_browsable_state", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "777", "ts3", 0, false, 0);
                Context.enum_EVALFLAGS evalFlags = Context.enum_EVALFLAGS.EVAL_NOFUNCEVAL;
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "<error>", "ts3", 0, true, evalFlags);

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct4 ts4 = new TestStruct4();

            int dummy4 = 4;                                     Label.Breakpoint("BREAK4");

            Label.Checkpoint("test_debugger_browsable_state", "test_NotifyOfCrossThreadDependency", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK4");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "666", "ts4", 0, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "888", "ts4", 1, false, 0);

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct5 ts5 = new TestStruct5();

            // part of NotifyOfCrossThreadDependency test, no active evaluation here for sure
            System.Diagnostics.Debugger.NotifyOfCrossThreadDependency();

            int dummy5 = 5;                                     Label.Breakpoint("BREAK5");

            Label.Checkpoint("test_NotifyOfCrossThreadDependency", "test_eval_timeout", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK5");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "111", "ts5", 0, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "<error>", "ts5", 1, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "\\\"text_333\\\"", "ts5", 2, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "<error>", "ts5", 3, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "555.5", "ts5", 4, false, 0);

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct6 ts6 = new TestStruct6();

            int dummy6 = 6;                                     Label.Breakpoint("BREAK6");

            Label.Checkpoint("test_eval_timeout", "test_eval_with_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK6");

                var task = System.Threading.Tasks.Task.Run(() => 
                {
                    Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "123", "ts6", 0, false, 0);
                    Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "<error>", "ts6", 1, false, 0);
                    Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "\\\"text_123\\\"", "ts6", 2, false, 0);
                });
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve) * 2 (for 2 command calls)
                if (!task.Wait(TimeSpan.FromSeconds(40)))
                    throw new DebuggerTimedOut(@"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct7 ts7 = new TestStruct7();

            int dummy7 = 7;                                     Label.Breakpoint("BREAK7");

            Label.Checkpoint("test_eval_with_exception", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK7");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "567", "ts7", 0, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "777", "ts7", 1, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "{System.DivideByZeroException}", "ts7", 2, false, 0);
                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "\\\"text_567\\\"", "ts7", 3, false, 0);

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        static void TestFunctionArgs(int test_arg_i, float test_arg_f, string test_arg_string)
        {
            int dummy1 = 1;                                     Label.Breakpoint("bp_func1");

            Label.Checkpoint("bp_func_test", "bp_func_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_func1");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_i", "10");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_f", "5");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_string", "\\\"test_string\\\"");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "test_arg_i", "20");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "test_arg_f", "50");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "test_arg_string", "\"edited_string\"", true);

                Context.Continue(@"__FILE__:__LINE__");
            });

            dummy1 = 2;                                         Label.Breakpoint("bp_func2");

            Label.Checkpoint("bp_func_test2", "test_eval_flags", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_func2");

                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_i", "20");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_f", "50");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "test_arg_string", "\\\"edited_string\\\"");

                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
