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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger { get; private set; }
    }
}

namespace MITestVarObject
{
    class Class1
    {
        public static int a;
        static Class1() { a = 100; }
        public Class1(int i) { a = i;}
    }

    class Class2
    {
        public static int a;
        public Class2() { a = 200; }
        public Class2(int i) { a = i;}
    }
    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK2");
                Context.Continue(@"__FILE__:__LINE__");
            });

            decimal d = 12345678901234567890123456m;
            decimal long_zero_dec = 0.00000000000000000017M;
            decimal short_zero_dec = 0.17M;
            int x = 1;                                                                          Label.Breakpoint("BREAK");

            Label.Checkpoint("values_test", "values_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK");

                var createDResult =
                    Context.MIDebugger.Request(String.Format("6-var-create - * \"{0}\"", "d"));
                Assert.Equal(MIResultClass.Done, createDResult.Class, @"__FILE__:__LINE__");

                string d_varName = ((MIConst)createDResult["name"]).CString;
                Assert.Equal("12345678901234567890123456", ((MIConst)createDResult["value"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("d", ((MIConst)createDResult["exp"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("0", ((MIConst)createDResult["numchild"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("decimal", ((MIConst)createDResult["type"]).CString, @"__FILE__:__LINE__");


                var createLongZeroDecResult =
                    Context.MIDebugger.Request(String.Format("7-var-create 8 * \"{0}\"", "long_zero_dec"));
                Assert.Equal(MIResultClass.Done, createLongZeroDecResult.Class, @"__FILE__:__LINE__");

                Assert.Equal("0.00000000000000000017", ((MIConst)createLongZeroDecResult["value"]).CString, @"__FILE__:__LINE__");


                var createShortZeroDecResult =
                    Context.MIDebugger.Request(String.Format("8-var-create 8 * \"{0}\"", "short_zero_dec"));
                Assert.Equal(MIResultClass.Done, createShortZeroDecResult.Class, @"__FILE__:__LINE__");

                Assert.Equal("0.17", ((MIConst)createShortZeroDecResult["value"]).CString, @"__FILE__:__LINE__");


                var notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "not_declared_variable"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class, @"__FILE__:__LINE__");

                notDeclaredVariable =
                    Context.MIDebugger.Request(String.Format("-var-create - * \"{0}\"", "not_declared_variable + 1"));
                Assert.Equal(MIResultClass.Error, notDeclaredVariable.Class, @"__FILE__:__LINE__");


                var attrDResult = Context.MIDebugger.Request("9-var-show-attributes " + d_varName);
                Assert.Equal(MIResultClass.Done, attrDResult.Class, @"__FILE__:__LINE__");
                Assert.Equal("editable", ((MIConst)attrDResult["status"]).CString, @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("10-var-delete " + d_varName).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request("11-var-show-attributes " + d_varName).Class,
                             @"__FILE__:__LINE__");

                var Class1var =
                    Context.MIDebugger.Request(String.Format("12-var-create - * \"{0}\"", "MITestVarObject.Class1.a"));
                Assert.Equal(MIResultClass.Done, Class1var.Class, @"__FILE__:__LINE__");

                string Class1varName = ((MIConst)Class1var["name"]).CString;
                Assert.Equal("100", ((MIConst)Class1var["value"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("MITestVarObject.Class1.a", ((MIConst)Class1var["exp"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("0", ((MIConst)Class1var["numchild"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("int", ((MIConst)Class1var["type"]).CString, @"__FILE__:__LINE__");

                var Class2var =
                    Context.MIDebugger.Request(String.Format("13-var-create - * \"{0}\"", "MITestVarObject.Class2.a"));
                Assert.Equal(MIResultClass.Done, Class2var.Class, @"__FILE__:__LINE__");

                string Class2varName = ((MIConst)Class2var["name"]).CString;
                Assert.Equal("0", ((MIConst)Class2var["value"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("MITestVarObject.Class2.a", ((MIConst)Class2var["exp"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("0", ((MIConst)Class2var["numchild"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("int", ((MIConst)Class2var["type"]).CString, @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Class2 class2 = new Class2();
            Class1.a = 1000;
            x = 2;                                                                              Label.Breakpoint("BREAK2");

            Label.Checkpoint("values_test2", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK2");

                var Class1var =
                    Context.MIDebugger.Request(String.Format("14-var-create - * \"{0}\"", "MITestVarObject.Class1.a"));
                Assert.Equal(MIResultClass.Done, Class1var.Class, @"__FILE__:__LINE__");

                string Class1varName = ((MIConst)Class1var["name"]).CString;
                Assert.Equal("1000", ((MIConst)Class1var["value"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("MITestVarObject.Class1.a", ((MIConst)Class1var["exp"]).CString, @"__FILE__:__LINE__");

                var Class2var =
                    Context.MIDebugger.Request(String.Format("15-var-create - * \"{0}\"", "MITestVarObject.Class2.a"));
                Assert.Equal(MIResultClass.Done, Class2var.Class, @"__FILE__:__LINE__");

                string Class2varName = ((MIConst)Class2var["name"]).CString;
                Assert.Equal("200", ((MIConst)Class2var["value"]).CString, @"__FILE__:__LINE__");
                Assert.Equal("MITestVarObject.Class2.a", ((MIConst)Class2var["exp"]).CString, @"__FILE__:__LINE__");

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
