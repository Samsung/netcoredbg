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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
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

        public void WasExceptionBreakpointHit(string caller_trace, string bpName)
        {
            var bp = (LineBreakpoint)ControlInfo.Breakpoints[bpName];

            Func<MIOutOfBandRecord, bool> filter = (record) => {
                if (!IsStoppedEvent(record)) {
                    return false;
                }

                var output = ((MIAsyncRecord)record).Output;
                var reason = (MIConst)output["reason"];

                if (reason.CString != "exception-received") {
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

                // we don't check exit code here, since Windows and Linux provide different exit code in case of "-gdb-exit" usage
                return true;
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
        public MIDebugger MIDebugger { get; private set; }
        public string id1 = null;
        public string id2 = null;
        public string id3 = null;
        public string id4 = null;
        public string id5 = null;
        public string id6 = null;
        public string id7 = null;
        public string id8 = null;
        public string id9 = null;
        public string id10 = null;
        public string id11 = null;
    }
}

namespace MITestExceptionBreakpoint
{
    public class AppException : Exception
    {
        public AppException(String message) : base(message) { }
        public AppException(String message, Exception inner) : base(message, inner) { }
    }

    public class Test
    {
        public void TestSystemException()
        {                                                                                   Label.Breakpoint("PIT_STOP_A");
            int zero = 1 - 1;
            try
            {
                int a = 1 / zero;                                                           Label.Breakpoint("EXCEPTION_A");
            }
            catch (Exception e)
            {
                Console.WriteLine($"Implicit Exception: {e.Message}");
            }
            try
            {
                throw new System.DivideByZeroException();                                   Label.Breakpoint("EXCEPTION_B");
            }
            catch (Exception e)
            {
                Console.WriteLine($"Explicit Exception: {e.Message}");
            }
            Console.WriteLine("Complete system exception test\n");
        }
        public void TestAppException()
        {                                                                                   Label.Breakpoint("PIT_STOP_B");
            try
            {
                CatchInner();
            }
            catch (AppException e)
            {
                Console.WriteLine("Caught: {0}", e.Message);
                if (e.InnerException != null)
                {
                    Console.WriteLine("Inner exception: {0}", e.InnerException);
                }
                throw new AppException("Error again in CatchInner caused by calling the ThrowInner method.", e);    Label.Breakpoint("EXCEPTION_E");
            }
            Console.WriteLine("Complete application exception test\n");
        }
        public void ThrowInner()
        {
            throw new AppException("Exception in ThrowInner method.");                      Label.Breakpoint("EXCEPTION_C");
        }
        public void CatchInner()
        {
            try
            {
                this.ThrowInner();
            }
            catch (AppException e)
            {
                throw new AppException("Error in CatchInner caused by calling the ThrowInner method.", e);          Label.Breakpoint("EXCEPTION_D");
            }
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "bp_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("3-break-exception-insert throw+user-unhandled A B C").Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request("4-break-exception-delete 4").Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("4-break-exception-delete 3 2 1").Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request("4-break-exception-delete 1").Class,
                             @"__FILE__:__LINE__");

                // Silent removing of previous global exception filter
                var insBp1Resp = Context.MIDebugger.Request("200-break-exception-insert throw *");
                Assert.Equal(MIResultClass.Done, insBp1Resp.Class, @"__FILE__:__LINE__");
                Context.id1 = ((MIConst)((MITuple)insBp1Resp["bkpt"])["number"]).CString;

                var insBp2Resp = Context.MIDebugger.Request("201-break-exception-insert throw+user-unhandled *");
                Assert.Equal(MIResultClass.Done, insBp2Resp.Class, @"__FILE__:__LINE__");
                Context.id2 = ((MIConst)((MITuple)insBp2Resp["bkpt"])["number"]).CString;

                Assert.Equal(MIResultClass.Error,
                             Context.MIDebugger.Request(String.Format("202-break-exception-delete {0}", Context.id1)).Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("203-break-exception-delete {0}", Context.id2)).Class,
                             @"__FILE__:__LINE__");
                //

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "PIT_STOP_A");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "PIT_STOP_B");
                Context.Continue(@"__FILE__:__LINE__");
            });

            Test test = new Test();

            // "State: !Thow() && !UserUnhandled() name := '*'";
            // Expected result => Not found any exception breakpoits.
            Label.Checkpoint("bp_test1", "bp_test2", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");
                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: !Thow() && UserUnhandled() name := '*'";
            // Expected result => Not found any exception breakpoits.
            Label.Checkpoint("bp_test2", "bp_test3", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");
                
                var insBp3Resp = Context.MIDebugger.Request("10-break-exception-insert user-unhandled *");
                Assert.Equal(MIResultClass.Done, insBp3Resp.Class, @"__FILE__:__LINE__");
                Context.id3 = ((MIConst)((MITuple)insBp3Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow() && !UserUnhandled() name := '*'";
            // Expected result => Raised EXCEPTION_A and EXCEPTION_B.
            Label.Checkpoint("bp_test3", "bp_test4", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("12-break-exception-delete {0}", Context.id3)).Class,
                             @"__FILE__:__LINE__");

                var insBp4Resp = Context.MIDebugger.Request("13-break-exception-insert throw *");
                Assert.Equal(MIResultClass.Done, insBp4Resp.Class, @"__FILE__:__LINE__");
                Context.id4 = ((MIConst)((MITuple)insBp4Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_A");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_B");

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow() && UserUnhandled() name := '*'";
            // Expected result => Raised EXCEPTION_A and EXCEPTION_B.
            Label.Checkpoint("bp_test4", "bp_test5", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("18-break-exception-delete {0}", Context.id4)).Class,
                             @"__FILE__:__LINE__");

                var insBp5Resp = Context.MIDebugger.Request("19-break-exception-insert throw+user-unhandled *");
                Assert.Equal(MIResultClass.Done, insBp5Resp.Class, @"__FILE__:__LINE__");
                Context.id5 = ((MIConst)((MITuple)insBp5Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_A");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_B");

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow() && UserUnhandled()";
            // "name := System.DivideByZeroException";
            // Expected result => Raised EXCEPTION_A and EXCEPTION_B.
            Label.Checkpoint("bp_test5", "bp_test6", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("23-break-exception-delete {0}", Context.id5)).Class,
                             @"__FILE__:__LINE__");

                var insBp6Resp = Context.MIDebugger.Request("24-break-exception-insert throw+user-unhandled System.DivideByZeroException");
                Assert.Equal(MIResultClass.Done, insBp6Resp.Class, @"__FILE__:__LINE__");
                Context.id6 = ((MIConst)((MITuple)insBp6Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_A");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_B");

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow()";
            // "name := System.DivideByZeroException";
            // "State: UserUnhandled()";
            // "name := System.DivideByZeroException";
            // Expected result => Raised EXCEPTION_A and EXCEPTION_B.
            Label.Checkpoint("bp_test6", "bp_test7", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("28-break-exception-delete {0}", Context.id6)).Class,
                             @"__FILE__:__LINE__");

                var insBp7Resp = Context.MIDebugger.Request("29-break-exception-insert throw System.DivideByZeroException");
                Assert.Equal(MIResultClass.Done, insBp7Resp.Class, @"__FILE__:__LINE__");
                Context.id7 = ((MIConst)((MITuple)insBp7Resp["bkpt"])["number"]).CString;

                var insBp8Resp = Context.MIDebugger.Request("30-break-exception-insert user-unhandled System.DivideByZeroException");
                Assert.Equal(MIResultClass.Done, insBp8Resp.Class, @"__FILE__:__LINE__");
                Context.id8 = ((MIConst)((MITuple)insBp8Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_A");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_B");

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow() && UserUnhandled()";
            // "name := System.DivideByZeroExceptionWrong and *";
            // Expected result => Raised EXCEPTION_A and EXCEPTION_B.
            Label.Checkpoint("bp_test7", "bp_test8", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("34-break-exception-delete {0}", Context.id7)).Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("35-break-exception-delete {0}", Context.id8)).Class,
                             @"__FILE__:__LINE__");

                var insBp9Resp = Context.MIDebugger.Request("36-break-exception-insert throw+user-unhandled *");
                Assert.Equal(MIResultClass.Done, insBp9Resp.Class, @"__FILE__:__LINE__");
                Context.id9 = ((MIConst)((MITuple)insBp9Resp["bkpt"])["number"]).CString;

                var insBp10Resp = Context.MIDebugger.Request("37-break-exception-insert throw+user-unhandled DivideByZeroExceptionWrong");
                Assert.Equal(MIResultClass.Done, insBp10Resp.Class, @"__FILE__:__LINE__");
                Context.id10 = ((MIConst)((MITuple)insBp10Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_A");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_B");

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();

            // "State: Thow() && UserUnhandled()";
            // "name := System.DivideByZeroExceptionWrong";
            // Expected result => Not found any exception breakpoits.
            Label.Checkpoint("bp_test8", "bp_test9", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_A");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("41-break-exception-delete {0}", Context.id9)).Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("42-break-exception-delete {0}", Context.id10)).Class,
                             @"__FILE__:__LINE__");

                var insBp11Resp = Context.MIDebugger.Request("36-break-exception-insert throw+user-unhandled System.DivideByZeroExceptionWrong");
                Assert.Equal(MIResultClass.Done, insBp11Resp.Class, @"__FILE__:__LINE__");
                Context.id11 = ((MIConst)((MITuple)insBp11Resp["bkpt"])["number"]).CString;

                Context.Continue(@"__FILE__:__LINE__");
            });
            test.TestSystemException();


            // "TODO:\n"
            // "Test for check forever waiting:\n"
            // "threads in NetcoreDBG (unsupported now)\n"
            // "Thread threadA = new Thread(new ThreadStart(test.TestSystemException));\n"
            // "Thread threadB = new Thread(new ThreadStart(test.TestSystemException));\n"
            // "threadA.Start();\n"
            // "threadB.Start();\n"
            // "threadA.Join();\n"
            // "threadB.Join();\n"

            // "State: !Thow() && UserUnhandled()";
            // "name := AppException";
            // Expected result => Raised EXCEPTION_C, EXCEPTION_D and exit after unhandled EXCEPTION_E.
            Label.Checkpoint("bp_test9", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "PIT_STOP_B");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request(String.Format("38-break-exception-delete {0}", Context.id11)).Class,
                             @"__FILE__:__LINE__");

                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("39-break-exception-insert unhandled MITestExceptionBreakpoint.AppException").Class,
                             @"__FILE__:__LINE__");
                Assert.Equal(MIResultClass.Done,
                             Context.MIDebugger.Request("40-break-exception-insert user-unhandled MITestExceptionBreakpoint.AppException").Class,
                             @"__FILE__:__LINE__");

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_C");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_D");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "EXCEPTION_E");

                // DO NOT call Context.Continue() here, in this way we prevent crash manager start in Tizen
            });
            // "Test with process exit at the end, need re-run"
            test.TestAppException();

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                // exit debugger and terminate (already crashed due to unhandled exception) test app
                Context.DebuggerExit(@"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
            });
        }
    }
}
