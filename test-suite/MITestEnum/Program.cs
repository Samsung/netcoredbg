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

        public void CheckEnum(string caller_trace, string VarName, string ExpectedResult)
        {
            var result = MIDebugger.Request(String.Format("-var-create - * \"{0}\"", VarName));
            Assert.Equal(MIResultClass.Done, result.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)result["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

namespace MITestEnum
{
    class Program
    {

        public enum enum1
        {
            read = 1,
            write = 2,
            append = 3
        }

        public enum enum2
        {
            append = 3,
            write = 2,
            read = 1,
            None = 0 // legit code
        }

        [Flags]
        public enum enum3
        {
            append = 4,
            write = 2,
            read = 1
        }

        [Flags]
        public enum enum4
        {
            read = 1,
            write = 2,
            append = 4,
            None = 0 // legit code
        }

        [Flags]
        public enum enum5
        {
            read = 1,
            write = 2,
            append = 3  // check for wrong code logic, not powers of two
        }

        [Flags]
        public enum enum6
        {
            append = 3, // check for wrong code logic, not powers of two
            write = 2,
            read = 1
        }

        public enum enum7 : byte
        {
            read = 1,
            write = 2
        }

        public enum enum8 : ushort
        {
            read = 1,
            write = 2
        }

        public enum enum9 : uint
        {
            read = 1,
            write = 2
        }

        public enum enum10 : ulong
        {
            read = 1,
            write = 2
        }

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "values_test", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "bp");
                Context.Continue(@"__FILE__:__LINE__");
            });

            // Note, we test for same behaviour as MS vsdbg have.

            enum1 enum1_test0 = (enum1)0;                 // check for wrong code logic, result = 0 - out of enumeration
            enum1 enum1_test1 = enum1.read;
            enum1 enum1_test2 = enum1.write;
            enum1 enum1_test3 = enum1.append;
            enum1 enum1_test4 = enum1.read & enum1.write; // check for wrong code logic, result = 0
            enum1 enum1_test5 = enum1.read | enum1.write; // check for wrong code logic, result = append - not flags enumeration can't be OR-ed
            enum1 enum1_test6 = (enum1)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum2 enum2_test0 = (enum2)0;                 // result = None
            enum2 enum2_test1 = enum2.read;
            enum2 enum2_test2 = enum2.write;
            enum2 enum2_test3 = enum2.append;
            enum2 enum2_test4 = enum2.read & enum2.write; // check for wrong code logic, result = None
            enum2 enum2_test5 = enum2.read | enum2.write; // check for wrong code logic, result = append - not flags enumeration can't be OR-ed
            enum2 enum2_test6 = (enum2)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum3 enum3_test0 = (enum3)0;                 // check for wrong code logic, result = 0 - out of enumeration
            enum3 enum3_test1 = enum3.read;
            enum3 enum3_test2 = enum3.write;
            enum3 enum3_test3 = enum3.append;
            enum3 enum3_test4 = enum3.read & enum3.write; // check for wrong code logic, result = 0 - out of enumeration
            enum3 enum3_test5 = enum3.read | enum3.write | enum3.append; // check that debugger care about enum sequence in case of flags attribute, result = read | write | append
            enum3 enum3_test6 = (enum3)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum4 enum4_test0 = (enum4)0;                 // result = None
            enum4 enum4_test1 = enum4.read;
            enum4 enum4_test2 = enum4.write;
            enum4 enum4_test3 = enum4.append;
            enum4 enum4_test4 = enum4.read & enum4.write; // check for wrong code logic, result = None
            enum4 enum4_test5 = enum4.read | enum4.write | enum4.append; // check that debugger care about enum sequence in case of flags attribute, result = read | write | append
            enum4 enum4_test6 = (enum4)101;               // check for wrong code logic, result = 101 - out of enumeration

            enum5 enum5_test1 = enum5.append;             // check that debugger care about enum sequence in case of flags attribute, result = append
            enum5 enum5_test2 = enum5.read | enum5.write; // check that debugger care about enum sequence in case of flags attribute, result = append
            enum5 enum5_test3 = enum5.write | enum5.read; // check that debugger care about enum sequence in case of flags attribute, result = append

            enum6 enum6_test1 = enum6.append;             // check that debugger care about enum sequence in case of flags attribute, result = append
            enum6 enum6_test2 = enum6.read | enum6.write; // check that debugger care about enum sequence in case of flags attribute, result = append
            enum6 enum6_test3 = enum6.write | enum6.read; // check that debugger care about enum sequence in case of flags attribute, result = append

            enum7 enum7_test1 = enum7.read;
            enum7 enum7_test2 = enum7.write;

            enum8 enum8_test1 = enum8.read;
            enum8 enum8_test2 = enum8.write;

            enum9 enum9_test1 = enum9.read;
            enum9 enum9_test2 = enum9.write;

            enum10 enum10_test1 = enum10.read;
            enum10 enum10_test2 = enum10.write;

            Console.WriteLine("A breakpoint \"bp\" is set on this line"); Label.Breakpoint("bp");

            Label.Checkpoint("values_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test0", "0");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test2", "write");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test3", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test4", "0");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test5", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum1_test6", "101");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test0", "None");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test2", "write");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test3", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test4", "None");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test5", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum2_test6", "101");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test0", "0");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test2", "write");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test3", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test4", "0");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test5", "read | write | append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum3_test6", "101");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test0", "None");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test2", "write");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test3", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test4", "None");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test5", "read | write | append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum4_test6", "101");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum5_test1", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum5_test2", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum5_test3", "append");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum6_test1", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum6_test2", "append");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum6_test3", "append");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum7_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum7_test2", "write");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum8_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum8_test2", "write");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum9_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum9_test2", "write");

                Context.CheckEnum(@"__FILE__:__LINE__", "enum10_test1", "read");
                Context.CheckEnum(@"__FILE__:__LINE__", "enum10_test2", "write");               

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
