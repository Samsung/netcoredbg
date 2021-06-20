using System;
using System.IO;
using System.Diagnostics;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void Prepare(string caller_trace)
        {
            // Explicitly disable JMC for this test.
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-gdb-set just-my-code 0").Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);

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

        public void AddExceptionBreakpoint(string caller_trace, string excStage, string excFilter)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-exception-insert " + excStage + " " + excFilter).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DeleteExceptionBreakpoint(string caller_trace, string excId)
        {
            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-exception-delete " + excId).Class,
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

        public void AbortExecution(string caller_trace)
        {
            Assert.Equal(MIResultClass.Done, MIDebugger.Request("-exec-abort").Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

                // we don't check exit code here, since Windows and Linux provide different exit code in case of unhandled exception
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
        MIDebugger MIDebugger;
    }
}

namespace MITestNoJMCExceptionBreakpoint
{
    class inside_user_code
    {
        static public void throw_Exception()
        {
            throw new System.Exception();                                                          Label.Breakpoint("bp3");
        }

        static public void throw_NullReferenceException()
        {
            throw new System.NullReferenceException();                                             Label.Breakpoint("bp4");
        }

        static public void throw_Exception_with_catch()
        {
            try {
                throw new System.Exception();                                                      Label.Breakpoint("bp1");
            } catch (Exception e) {}
        }

        static public void throw_Exception_NullReferenceException_with_catch()
        {
            try {
                throw new System.Exception();
            } catch {}

            try {
                throw new System.NullReferenceException();                                         Label.Breakpoint("bp2");
            } catch {}
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class outside_user_code
    {
        static public void throw_Exception()
        {
            throw new System.Exception();                                                          Label.Breakpoint("bp5");
        }

        static public void throw_NullReferenceException()
        {
            throw new System.NullReferenceException();                                             Label.Breakpoint("bp8");
        }

        static public void throw_Exception_with_catch()
        {
            try {
                throw new System.Exception();                                                      Label.Breakpoint("bp6");
            } catch {}
        }

        static public void throw_Exception_NullReferenceException_with_catch()
        {
            try {
                throw new System.Exception();
            } catch {}

            try {
                throw new System.NullReferenceException();                                         Label.Breakpoint("bp7");
            } catch {}
        }
    }

    class inside_user_code_wrapper
    {
        static public void call(Action callback)
        {
            callback();
        }

        static public void call_with_catch(Action callback)
        {
            try {
                callback();
            } catch {};
        }
    }

    [DebuggerNonUserCodeAttribute()]
    class outside_user_code_wrapper
    {
        static public void call(Action callback)
        {
            callback();
        }

        static public void call_with_catch(Action callback)
        {
            try {
                callback();
            } catch {};
        }
    }

    class Program
    {
        static void Main(string[] args)
        {
            Label.Checkpoint("init", "test_throw_all", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");

                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_1");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_2");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_3");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_4");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_5");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_6");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_7");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_8");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_9");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_10");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_11");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_12");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_13");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_14");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_15");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_16");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_17");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_18");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_19");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp_test_20");

                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "*");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "throw *"

            inside_user_code.throw_Exception_with_catch();                                         Label.Breakpoint("bp_test_1");
            try {
                outside_user_code.throw_Exception();                                               Label.Breakpoint("bp_test_2");
            } catch {};
            outside_user_code.throw_Exception_with_catch();                                        Label.Breakpoint("bp_test_3");

            Label.Checkpoint("test_throw_all", "test_throw_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_1");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp1", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_2");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp5", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_3");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp6", "clr", "throw", "System.Exception");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "21");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.NullReferenceException");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "throw System.NullReferenceException"

            inside_user_code.throw_Exception_NullReferenceException_with_catch();                  Label.Breakpoint("bp_test_4");
            outside_user_code.throw_Exception_NullReferenceException_with_catch();                 Label.Breakpoint("bp_test_5");
            try {
                outside_user_code.throw_Exception();
            } catch {};
            try {
                outside_user_code.throw_NullReferenceException();                                  Label.Breakpoint("bp_test_6");
            } catch {};

            Label.Checkpoint("test_throw_concrete_exception", "test_user_unhandled_all", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_4");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp2", "clr", "throw", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_5");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp7", "clr", "throw", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_6");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp8", "clr", "throw", "System.NullReferenceException");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "22");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "user-unhandled", "*");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "user-unhandled *" (also test, that "throw" don't emit break event after breakpoint removed)
            // Must emit break event only in case catch block outside of user code, but "throw" inside user code.

            inside_user_code.throw_Exception_with_catch();
            try {
                outside_user_code.throw_Exception();
            } catch {};
            outside_user_code.throw_Exception_with_catch();

            try {
                outside_user_code_wrapper.call(inside_user_code.throw_Exception);
            } catch {};
            try {
                outside_user_code_wrapper.call(outside_user_code.throw_Exception);
            } catch {};
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);           Label.Breakpoint("bp_test_7");
            outside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);          Label.Breakpoint("bp_test_8");

            try {
                inside_user_code_wrapper.call(outside_user_code.throw_Exception);
            } catch {};
            try {
                inside_user_code_wrapper.call(inside_user_code.throw_Exception);
            } catch {};
            inside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);
            inside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);

            Label.Checkpoint("test_user_unhandled_all", "test_user_unhandled_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_7");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_8");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "23");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "user-unhandled", "System.NullReferenceException");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "user-unhandled System.NullReferenceException"

            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_NullReferenceException);
            Console.WriteLine("end");                                                              Label.Breakpoint("bp_test_9");

            Label.Checkpoint("test_user_unhandled_concrete_exception", "test_throw_user_unhandled_all", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_9");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "24");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "*");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "throw+user-unhandled *"

            inside_user_code.throw_Exception_with_catch();                                         Label.Breakpoint("bp_test_10");
            try {
                outside_user_code.throw_Exception();                                               Label.Breakpoint("bp_test_11");
            } catch {};
            outside_user_code.throw_Exception_with_catch();                                        Label.Breakpoint("bp_test_12");
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);           Label.Breakpoint("bp_test_13");
            outside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);          Label.Breakpoint("bp_test_14");
            Console.WriteLine("end");                                                              Label.Breakpoint("bp_test_15");

            Label.Checkpoint("test_throw_user_unhandled_all", "test_throw_user_unhandled_concrete_exception", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_10");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp1", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_11");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp5", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_12");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp6", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_13");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp3", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_14");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp5", "clr", "throw", "System.Exception");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_15");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "25");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "System.NullReferenceException");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "throw+user-unhandled System.NullReferenceException"

            inside_user_code.throw_Exception_with_catch();
            try {
                outside_user_code.throw_Exception();
            } catch {};
            outside_user_code.throw_Exception_with_catch();
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);
            outside_user_code_wrapper.call_with_catch(outside_user_code.throw_Exception);
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_NullReferenceException);   Label.Breakpoint("bp_test_16");
            Console.WriteLine("end");                                                                   Label.Breakpoint("bp_test_17");

            Label.Checkpoint("test_throw_user_unhandled_concrete_exception", "test_mi_protocol", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_16");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "clr", "throw", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_17");

                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "26");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "user-unhandled", "*");
                // Important! "unhandled" must be allowed.
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "unhandled", "System.AppDomainUnloadedException System.Threading.ThreadAbortException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.ArgumentNullException System.Exception System.NullReferenceException System.ArgumentOutOfRangeException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "System.Windows.Markup.XamlParseException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "System.Reflection.MissingMetadataException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "System.NullReferenceException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw+user-unhandled", "Microsoft.UI.Xaml.Markup.XamlParseException Microsoft.UI.Xaml.Markup.XamlParseException");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "--mda throw", "CallbackOnCollectedDelegate ContextSwitchDeadlock RaceOnRCWCleanup Reentrancy");
                // Note, this is wrong setup for MDA that don't have "System.Exception", but in this way we test MDA/CLR category work.
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "--mda throw", "System.Exception");
                // Delete only "System.Exception" from "throw" (previously had configured 4 breakpoints by one "throw" command).
                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "31");
                // Check for breakpoint ID (test CalculateExceptionBreakpointHash() work), if we have 43 here -
                // all breakpoint were created in proper way, no ID was reused (all breakpoint have unique hash at creation time).
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "43");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // test MI/GDB add multiple breakpoints

            inside_user_code.throw_Exception_with_catch();
            try {
                outside_user_code.throw_Exception();
            } catch {};
            outside_user_code.throw_Exception_with_catch();
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_Exception);                Label.Breakpoint("bp_test_18");
            outside_user_code_wrapper.call_with_catch(inside_user_code.throw_NullReferenceException);   Label.Breakpoint("bp_test_19");

            Label.Checkpoint("test_mi_protocol", "test_unhandled", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_18");

                // Final check for breakpoint ID (test CalculateExceptionBreakpointHash() work), if we have 44 here -
                // all breakpoint were created in proper way, no ID was reused (all breakpoint have unique hash at creation time) except code below.
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.AddExceptionBreakpoint(@"__FILE__:__LINE__", "throw", "System.Exception");
                Context.DeleteExceptionBreakpoint(@"__FILE__:__LINE__", "44");

                Context.Continue(@"__FILE__:__LINE__");
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_19");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp4", "clr", "throw", "System.NullReferenceException");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // test "unhandled"

            throw new System.ArgumentOutOfRangeException();                                             Label.Breakpoint("bp_test_20");

            Label.Checkpoint("test_unhandled", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp_test_20");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_20", "clr", "throw", "System.ArgumentOutOfRangeException");
                Context.Continue(@"__FILE__:__LINE__");
                Context.WasExceptionBreakpointHit(@"__FILE__:__LINE__", "bp_test_20", "clr", "unhandled", "System.ArgumentOutOfRangeException");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                // At this point debugger stops at unhandled exception, no reason continue process, abort execution.
                Context.AbortExecution( @"__FILE__:__LINE__");
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
