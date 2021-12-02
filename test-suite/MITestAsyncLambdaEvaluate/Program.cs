using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

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

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            MIDebugger = new MIDebugger(debuggerClient);
        }

        ControlInfo ControlInfo;
        public MIDebugger MIDebugger { get; private set; }
    }
}

namespace MITestAsyncLambdaEvaluate
{
    class TestWithThis
    {
        delegate void Lambda1(string argVar);
        delegate void Lambda2(int i);
        delegate void Lambda3();

        int field_i = 33;

        public static int test_funct1()
        {
            return 1;
        }

        public int test_funct2()
        {
            return 2;
        }

        public void FuncLambda(int arg_i)
        {
            int local_i = 10;                                                                      Label.Breakpoint("bp12");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp13");
            }

            String mainVar = "mainVar";
            Lambda1 lambda1 = (argVar) => {
                string localVar = "localVar";                                                      Label.Breakpoint("bp14");
                Console.WriteLine(arg_i.ToString() + argVar + mainVar + localVar + field_i.ToString());
            };
            lambda1("argVar");

            Lambda2 lambda2 = (i) => {
                test_funct1();                                                                     Label.Breakpoint("bp15");
                test_funct2();
            };
            lambda2(5);

            Lambda3 lambda3 = () => {
                Console.WriteLine("none");                                                         Label.Breakpoint("bp16");
            };
            lambda3();

            Label.Checkpoint("func_lambda", "func_async", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp12");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda2", "lambda2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda3", "lambda3");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp13");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda2", "lambda2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda3", "lambda3");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp14");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp15");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp16");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "field_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "arg_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        public async Task FuncAsync(int arg_i)
        {
            await Task.Delay(500);

            int local_i = 10;                                                                      Label.Breakpoint("bp17");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp18");
            }

            Label.Checkpoint("func_async", "func_async_with_lambda", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp17");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp18");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        public async Task FuncAsyncWithLambda(int arg_i)
        {
            await Task.Delay(500);

            String mainVar = "mainVar";
            Lambda1 lambda1 = (argVar) => {
                string localVar = "localVar";                                                      Label.Breakpoint("bp19");
                Console.WriteLine(argVar + mainVar + localVar);

                int mainVar2 = 5;
                Lambda2 lambda2 = (i) => {
                    string localVar2 = "localVar";                                                 Label.Breakpoint("bp20");
                    Console.WriteLine(arg_i.ToString() + argVar + mainVar + localVar + mainVar2.ToString() + i.ToString() + localVar2 + field_i.ToString());
                };
                lambda2(5);

                Lambda3 lambda3 = () => {
                    test_funct1();                                                                 Label.Breakpoint("bp21");
                    test_funct2();
                };
                lambda3();

                Lambda3 lambda4 = () => {
                    Console.WriteLine("none");                                                     Label.Breakpoint("bp22");
                };
                lambda4();
            };
            lambda1("argVar");

            int local_i = 10;                                                                      Label.Breakpoint("bp23");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp24");
            }

            Label.Checkpoint("func_async_with_lambda", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp19");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda2", "lambda2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda3", "lambda3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda3", "lambda4");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "mainVar2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp20");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"localVar\\\"", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "mainVar2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda4", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp21");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda4", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp22");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "field_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "arg_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda4", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp23");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis.Lambda1}", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda4", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp24");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis}", "MITestAsyncLambdaEvaluate.TestWithThis", "this");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "field_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.TestWithThis.Lambda1}", "MITestAsyncLambdaEvaluate.TestWithThis.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda4", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "test_funct2()");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }

    class Program
    {
        delegate void Lambda1(string argVar);
        delegate void Lambda2(int i);
        delegate void Lambda3();

        public static int test_funct1()
        {
            return 1;
        }

        public int test_funct2()
        {
            return 2;
        }

        static void FuncLambda(int arg_i)
        {
            int local_i = 10;                                                                      Label.Breakpoint("bp1");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp2");
            }

            String mainVar = "mainVar";
            Lambda1 lambda1 = (argVar) => {
                string localVar = "localVar";                                                      Label.Breakpoint("bp3");
                Console.WriteLine(arg_i.ToString() + argVar + mainVar + localVar);
            };
            lambda1("argVar");

            Lambda3 lambda3 = () => {
                Console.WriteLine("none");                                                         Label.Breakpoint("bp4");
            };
            lambda3();

            Label.Checkpoint("static_func_lambda", "static_func_async", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda3", "lambda3");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda3", "lambda3");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp4");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "arg_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static async Task FuncAsync(int arg_i)
        {
            await Task.Delay(500);

            int local_i = 10;                                                                      Label.Breakpoint("bp5");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp6");
            }

            Label.Checkpoint("static_func_async", "static_func_async_with_lambda", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp5");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp6");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "50", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static async Task FuncAsyncWithLambda(int arg_i)
        {
            await Task.Delay(500);

            String mainVar = "mainVar";
            Lambda1 lambda1 = (argVar) => {
                string localVar = "localVar";                                                      Label.Breakpoint("bp7");
                Console.WriteLine(argVar + mainVar + localVar);

                int mainVar2 = 5;
                Lambda2 lambda2 = (i) => {
                    string localVar2 = "localVar";                                                 Label.Breakpoint("bp8");
                    Console.WriteLine(arg_i.ToString() + argVar + mainVar + localVar + mainVar2.ToString() + i.ToString() + localVar2);
                };
                lambda2(5);

                Lambda3 lambda3 = () => {
                    Console.WriteLine("none");                                                     Label.Breakpoint("bp9");
                };
                lambda3();
            };
            lambda1("argVar");

            int local_i = 10;                                                                      Label.Breakpoint("bp10");

            {
                int scope_i = 20;                                                                  Label.Breakpoint("bp11");
            }

            Label.Checkpoint("static_func_async_with_lambda", "func_lambda", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp7");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda2", "lambda2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "MITestAsyncLambdaEvaluate.Program.Lambda3", "lambda3");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "mainVar2");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp8");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "null", "string", "localVar2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"argVar\\\"", "string", "argVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"localVar\\\"", "string", "localVar");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "5", "int", "mainVar2");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp9");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "arg_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "local_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda1", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp10");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.Program.Lambda1}", "MITestAsyncLambdaEvaluate.Program.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "scope_i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");

                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp11");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "100", "int", "arg_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestAsyncLambdaEvaluate.Program.Lambda1}", "MITestAsyncLambdaEvaluate.Program.Lambda1", "lambda1");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "10", "int", "local_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "scope_i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"mainVar\\\"", "string", "mainVar");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "i", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "argVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "localVar", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "mainVar2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda2", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lambda3", "error");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "this", "error");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "test_funct1()");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "test_funct2()", "Error");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "static_func_lambda", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp5");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp6");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp7");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp8");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp9");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp10");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp11");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp12");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp13");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp14");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp15");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp16");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp17");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp18");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp19");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp20");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp21");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp22");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp23");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp24");
                Context.Continue(@"__FILE__:__LINE__");
            });

            FuncLambda(10);
            FuncAsync(50).Wait();
            FuncAsyncWithLambda(100).Wait();

            TestWithThis testWithThis = new TestWithThis();
            testWithThis.FuncLambda(10);
            testWithThis.FuncAsync(50).Wait();
            testWithThis.FuncAsyncWithLambda(100).Wait();
 
            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
