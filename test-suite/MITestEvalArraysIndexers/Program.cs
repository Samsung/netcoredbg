using System;
using System.IO;
using System.Collections.Generic;

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

        public void EnableBreakpoint(string caller_trace, string bpName)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];

            Assert.Equal(BreakpointType.Line, bp.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);

            var lbp = (LineBreakpoint)bp;

            Assert.Equal(MIResultClass.Done,
                         MIDebugger.Request("-break-insert -f " + lbp.FileName + ":" + lbp.NumLine).Class,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CalcAndCheckExpression(string caller_trace, string ExpectedResult, string expr)
        {
            var res = MIDebugger.Request("-var-create - * \"" + expr + "\"");

            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedResult, ((MIConst)res["value"]).CString, @"__FILE__:__LINE__"+"\n"+caller_trace);
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

        public void GetResultAsString(string caller_trace, string expr, out string strRes)
        {
            var res = MIDebugger.Request("-var-create - * \"" + expr + "\"");
            Assert.Equal(MIResultClass.Done, res.Class, @"__FILE__:__LINE__"+"\n" + caller_trace);
            strRes = ((MIConst)res["value"]).CString;
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

namespace MITestEvalArraysIndexers
{
    public class SimpleInt
    {
        // Array of temperature values
        int[] ints = new int[10]
        {
            0, 11, 22, 33, 44, 55, 66, 77, 88, 99
        };

        // To enable client code to validate input
        // when accessing your indexer.
        public int flLength => ints.Length;

        // Indexer declaration.
        // If index is out of range, the temps array will throw the exception.
        public int this[int index]
        {
            get => ints[index];
            set => ints[index] = value;
        }
    }

    public class TwoDimInt : SimpleInt
    {
        int[,] ints2dim = new int[5,5]
        {
            {0, 1, 2, 3, 4},
            {5, 6, 7, 8, 9},
            {10, 11, 12, 13, 14},
            {15, 16, 17, 18, 19},
            {20, 21, 22, 23, 24}
        };

        public int intLength => ints2dim.Length;

        public int this[int i, int j]
        {
            get => ints2dim[i, j];
            set => ints2dim[i, j] = value;
        }
    }

    public class IndexAsString : TwoDimInt
    {
        internal static Dictionary<string, int> digits = new Dictionary<string, int>
        {
            {"zero", 0},
            {"one", 1},
            {"two", 2},
            {"three", 3},
            {"four", 4},
            {"five", 5},
            {"six", 6},
            {"seven", 7},
            {"eight", 8},
            {"nine", 9}
        };

        public int this [int i, string s]
        {
            get => Multiply(i, s);
        }

        private int Multiply(int i, string s)
        {
            return digits[s] * i;
        }
    }

    public class MyString
    {
        string s;
        public MyString(string ms)
        {
            s = ms;
        }
    }

    struct MyInt
    {
        int i;
        public MyInt(int mi)
        {
            i = mi;
        }
    }

    class Program
    {
        static string[] str = new string[]
        {
            "zero", "one", "two", "three", "four",
            "five", "six", "seven", "eight", "nine", "ten"
        };

        static void Main(string[] args)
        {
            Label.Checkpoint("init", "expression_test1", (Object context) => {
                Context Context = (Context)context;
                Context.Prepare(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
                Context.EnableBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.Continue(@"__FILE__:__LINE__");
            });

            int[,] multiArray =
            {
                { 101, 102, 103},
                { 104, 105, 106}
            };

            var simpleInt = new SimpleInt();
            var twoDimInt = new TwoDimInt();
            var indexAsString = new IndexAsString();
            Dictionary<int,string> dictis = new Dictionary<int,string>();
            Dictionary<string,int> dictsi = new Dictionary<string,int>();
            Dictionary<MyInt,MyString> dictmims = new Dictionary<MyInt,MyString>();
            Dictionary<MyString,MyInt> dictmsmi = new Dictionary<MyString,MyInt>();
            List<string> lists = new List<string>();
            List<MyString> listms = new List<MyString>();
            SortedList<string,int> slist = new SortedList<string,int>();

            MyInt[] myInts = new MyInt[6]
                {new MyInt(0), new MyInt(1), new MyInt(2), new MyInt(3), new MyInt(4), new MyInt(5)};
            MyString[] myStrings = new MyString[6]
                {new MyString("zero"), new MyString("one"), new MyString("two"), new MyString("three"), new MyString("four"), new MyString("five")};

            int i0 = 0;
            int i1 = 1;
            int i2 = 2;
            int i4 = 4;
            int i7 = 7;
            int i11 = 11;

            dictis.Add(0, "zero");
            dictis.Add(1, "one");
            dictis.Add(2, "two");
            dictis.Add(3, "three");
            dictis.Add(4, "four");

            dictsi.Add("zero", 0);
            dictsi.Add("one", 1);
            dictsi.Add("two", 2);
            dictsi.Add("three", 3);
            dictsi.Add("four", 4);

            lists.Add("zero");
            lists.Add("one");
            lists.Add("two");
            lists.Add("three");
            lists.Add("four");

            listms.Add(myStrings[0]);
            listms.Add(myStrings[1]);
            listms.Add(myStrings[2]);
            listms.Add(myStrings[3]);
            listms.Add(myStrings[4]);

            slist.Add("zero", 0);
            slist.Add("one", 1);
            slist.Add("two", 2);
            slist.Add("three", 3);
            slist.Add("four", 4);

            dictmims.Add(myInts[0],myStrings[0]);
            dictmims.Add(myInts[1],myStrings[1]);
            dictmims.Add(myInts[2],myStrings[2]);
            dictmims.Add(myInts[3],myStrings[3]);
            dictmims.Add(myInts[4],myStrings[4]);

            dictmsmi.Add(myStrings[0],myInts[0]);
            dictmsmi.Add(myStrings[1],myInts[1]);
            dictmsmi.Add(myStrings[2],myInts[2]);
            dictmsmi.Add(myStrings[3],myInts[3]);
            dictmsmi.Add(myStrings[4],myInts[4]);

            // Use the indexer's set accessor
            simpleInt[3] = 333;
            simpleInt[5] = 555;
            twoDimInt[1,1] = 111;
            twoDimInt[2,2] = 222;
            twoDimInt[4,4] = 444;
            indexAsString[3,3] = 333;
            indexAsString[0,0] = 100;
            simpleInt[6] = 66;                                       Label.Breakpoint("BREAK1");

            Label.Checkpoint("expression_test1", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{int[2, 3]}", "int[,]", "multiArray");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "101", "int", "multiArray[0,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "105", "int", "multiArray[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "104", "int", "multiArray[1,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "102", "int", "multiArray[0,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "104", "int", "multiArray[i1,i0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "102", "int", "multiArray[i0,i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "101", "int", "multiArray[ 0 , 0 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "105", "int", "multiArray  [ 1,1 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "simpleInt[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "simpleInt[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "simpleInt[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "333", "int", "simpleInt[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "simpleInt[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "simpleInt[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "simpleInt[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "simpleInt[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "simpleInt[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "555", "int", "simpleInt[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "66", "int", "simpleInt[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "simpleInt[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "simpleInt[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "88", "int", "simpleInt[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "99", "int", "simpleInt[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "simpleInt[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "simpleInt[i11]");

                // check twoDimInt (child of simpleInt) indexer
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "twoDimInt[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "twoDimInt[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "twoDimInt[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "twoDimInt[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "twoDimInt[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "twoDimInt[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "twoDimInt[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "twoDimInt[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "twoDimInt[ i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "twoDimInt[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "twoDimInt[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "66", "int", "twoDimInt[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "twoDimInt[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "twoDimInt[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "88", "int", "twoDimInt[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "99", "int", "twoDimInt[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i11]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[ 1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[1,1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[1, 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[ 1, 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "111", "int", "twoDimInt[ 1 , 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "222", "int", "twoDimInt[2,2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "444", "int", "twoDimInt[4,4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "9", "int", "twoDimInt[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "19", "int", "twoDimInt[i1+i2,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i1+i4,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i1, i2+i4]");

                //check indexAsString
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "0", "int", "indexAsString[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "11", "int", "indexAsString[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "22", "int", "indexAsString[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "33", "int", "indexAsString[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "indexAsString[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "44", "int", "indexAsString[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "indexAsString[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "indexAsString[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "indexAsString[ i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "indexAsString[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "55", "int", "indexAsString[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "66", "int", "indexAsString[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "indexAsString[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "77", "int", "indexAsString[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "88", "int", "indexAsString[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "99", "int", "indexAsString[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i11]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[ 1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[1,1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[1, 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[ 1, 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "6", "int", "indexAsString[ 1 , 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "12", "int", "indexAsString[2,2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "24", "int", "indexAsString[4,4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "9", "int", "indexAsString[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "19", "int", "indexAsString[i1+i2,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i1+i4,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i1, i2+i4]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "indexAsString[1,str[1]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "indexAsString[2, str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "9", "int", "indexAsString[3,str[3] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "16", "int", "indexAsString[4, str[4]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "25", "int", "indexAsString[ 5, str[5] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "36", "int", "indexAsString[ 6 , str[6] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "indexAsString[11,str[10]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "indexAsString[i1, str[10]]");

                // check Dictionary<int,string>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"one\\\"", "string", "dictis[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[ 4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[ 4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[ i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictis[ i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[ i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[ i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[ i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[i1 + i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "dictis[ i1 + i2 ]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictis[\\\"four\\\"]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictis[str[4]]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictis[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictis[i7]");
 
                // check Dictionary<string, int>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "dictsi[\\\"one\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "dictsi[ \\\"one\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "dictsi[\\\"one\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "dictsi[ \\\"one\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[\\\"four\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[ \\\"four\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[\\\"four\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[ \\\"four\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "dictsi[str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "dictsi[str[ 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "dictsi[str[2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "dictsi[str[ 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[ 2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[ 2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[ 2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[2 + 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictsi[str[ 2 + 2 ]]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictsi[4]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictsi[i4]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictsi[str[5]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictsi[\\\"five\\\"]");

                // check List<string>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"one\\\"", "string", "lists[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[ 4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[ 4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[ i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "lists[ i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[ i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[ i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[ i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[i1 + i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "lists[ i1 + i2 ]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists[4.4]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists[str[4]]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "lists[\\\"four\\\"]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "lists[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "lists[i7]");

                // check List<MyString>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvalArraysIndexers.MyString}", "MITestEvalArraysIndexers.MyString", "listms[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvalArraysIndexers.MyString}", "MITestEvalArraysIndexers.MyString", "listms[ 3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvalArraysIndexers.MyString}", "MITestEvalArraysIndexers.MyString", "listms[3 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{MITestEvalArraysIndexers.MyString}", "MITestEvalArraysIndexers.MyString", "listms[ 3 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"one\\\"", "string", "listms[1].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[4].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[ 4].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[4 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[ 4 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[i4].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[ i4].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[i4 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "listms[ i4 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[i1+i2].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[ i1+i2].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[i1+i2 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[ i1+i2 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[i1 + i2].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[ i1 + i2].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[i1 + i2 ].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"three\\\"", "string", "listms[ i1 + i2 ].s");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "listms[4.4]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "listms[myStrings[4]]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "listms[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "listms[i7]");

                // check SortedList<string,int>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "slist[\\\"one\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "slist[ \\\"one\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "slist[\\\"one\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "slist[ \\\"one\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[\\\"four\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[ \\\"four\\\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[\\\"four\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[ \\\"four\\\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "slist[str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "slist[str[ 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "slist[str[2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "2", "int", "slist[str[ 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[ 2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[ 2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[ 2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[2 + 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "slist[str[ 2 + 2 ]]");

                // check Dictionary<MyInt,MyString>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"one\\\"", "string", "dictmims[myInts[1]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictmims[myInts[4]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictmims[ myInts[4]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictmims[myInts[4]].s ");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "\\\"four\\\"", "string", "dictmims[ myInts[4]].s ");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictmims[myStrings[4]]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictmims[\\\"a string\\\"]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictmims[myInts[5]]");

                // check Dictionary<MyInt,MyString>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "1", "int", "dictmsmi[myStrings[1]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictmsmi[myStrings[4]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictmsmi[ myStrings[4]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictmsmi[myStrings[4]].i ");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "4", "int", "dictmsmi[ myStrings[4]].i ");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictmsmi[myInts[4]]", "Error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", "dictmsmi[\\\"a string\\\"]", "Error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictmsmi[myStrings[5]]");

                Context.Continue(@"__FILE__:__LINE__");
            });

            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }

        struct TestStruct
        {
            public int a;
            public int b;

            public TestStruct(int x, int y)
            {
                a = x;
                b = y;
            }
        }
    }
}
