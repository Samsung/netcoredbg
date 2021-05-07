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

        public void CreateAndAssignVar(string caller_trace, string variable, string val)
        {
            var res = MIDebugger.Request("-var-create - * \"" + variable + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string internalName = ((MIConst)res["name"]).CString;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-var-assign " + internalName + " " + "\"" + val + "\"").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CreateAndCompareVar(string caller_trace, string variable, string val)
        {
            var res = MIDebugger.Request("-var-create - * \"" + variable + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(val, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetAndCheckChildValue(string caller_trace, string ExpectedResult, string variable,
                                          int childIndex, bool setEvalFlags, enum_EVALFLAGS evalFlags)
        {
            var res = MIDebugger.Request("-var-create - * " +
                                         "\"" + variable + "\"" +
                                         (setEvalFlags ? (" --evalFlags " + (int)evalFlags) : "" ));
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            string struct2 = ((MIConst)res["name"]).CString;

            res = MIDebugger.Request("-var-list-children --simple-values " +
                                     "\"" + struct2 + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var children = (MIList)res["children"];
            var child =  (MITuple)((MIResult)children[childIndex]).Value;

            Assert.Equal(ExpectedResult, ((MIConst)child["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
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
    public struct TestStruct1
    {
        public int val1;
        public byte val2;

        public TestStruct1(int v1, byte v2)
        {
            val1 = v1;
            val2 = v2;
        }
    }

    public struct TestStruct2
    {
        public int val1;
        public TestStruct1 struct2;

        public TestStruct2(int v1, int v2, byte v3)
        {
            val1 = v1;
            struct2.val1 = v2;
            struct2.val2 = v3;
        }
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

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct2 ts = new TestStruct2(1, 5, 10);

            bool testBool = false;
            char testChar = 'ㅎ';
            byte testByte = (byte)10;
            sbyte testSByte = (sbyte)-100;
            short testShort = (short)-500;
            ushort testUShort = (ushort)500;
            int testInt = -999999;
            uint testUInt = 999999;
            long testLong = -999999999;
            ulong testULong = 9999999999;

            decimal b = 0000001.000000000000000000000000006M;
            int[] arrs = decimal.GetBits(b);
            string testString = "someNewString that I'll test with";

            int dummy1 = 1;                                     Label.Breakpoint("BREAK1");

            Label.Checkpoint("setup_var", "test_var", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ts.struct2.val1", "666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testBool", "true");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testChar", "'a'");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testByte", "200");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testSByte", "-1");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testShort", "-666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testUShort", "666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testInt", "666666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testUInt", "666666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testLong", "-666666666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testULong", "666666666");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "b", "-1.000000000000000000000017M");
                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "testString", "\"edited string\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            int dummy2 = 2;                                     Label.Breakpoint("BREAK2");

            Label.Checkpoint("test_var", "test_eval_flags", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                Context.GetAndCheckChildValue(@"__FILE__:__LINE__", "666", "ts.struct2", 0, false, 0);
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testBool", "true");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testChar", "97 'a'");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testByte", "200");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testSByte", "-1");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testShort", "-666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testUShort", "666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testInt", "666666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testUInt", "666666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testLong", "-666666666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testULong", "666666666");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "b", "-1.000000000000000000000017");
                Context.CreateAndCompareVar(@"__FILE__:__LINE__", "testString", "\\\"edited string\\\"");

                Context.Continue(@"__FILE__:__LINE__");
            });

            TestStruct3 ts3 = new TestStruct3();

            int dummy3 = 3;                                     Label.Breakpoint("BREAK3");

            Label.Checkpoint("test_eval_flags", "test_debugger_browsable_state", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK3");

                Context.CreateAndAssignVar(@"__FILE__:__LINE__", "ts3.val1", "666");
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
                // we have 5 seconds evaluation timeout by default, wait 20 seconds (5 seconds eval timeout * 3 eval requests + 5 seconds reserve)
                if (!task.Wait(TimeSpan.FromSeconds(20)))
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
    }
}
