using System;
using System.IO;
using System.Collections.Generic;

using NetcoreDbgTest;
using NetcoreDbgTest.VSCode;
using NetcoreDbgTest.Script;

using Newtonsoft.Json;

namespace NetcoreDbgTest.Script
{
    class Context
    {
        public void PrepareStart(string caller_trace)
        {
            InitializeRequest initializeRequest = new InitializeRequest();
            initializeRequest.arguments.clientID = "vscode";
            initializeRequest.arguments.clientName = "Visual Studio Code";
            initializeRequest.arguments.adapterID = "coreclr";
            initializeRequest.arguments.pathFormat = "path";
            initializeRequest.arguments.linesStartAt1 = true;
            initializeRequest.arguments.columnsStartAt1 = true;
            initializeRequest.arguments.supportsVariableType = true;
            initializeRequest.arguments.supportsVariablePaging = true;
            initializeRequest.arguments.supportsRunInTerminalRequest = true;
            initializeRequest.arguments.locale = "en-us";
            Assert.True(VSCodeDebugger.Request(initializeRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);

            LaunchRequest launchRequest = new LaunchRequest();
            launchRequest.arguments.name = ".NET Core Launch (console) with pipeline";
            launchRequest.arguments.type = "coreclr";
            launchRequest.arguments.preLaunchTask = "build";
            launchRequest.arguments.program = ControlInfo.TargetAssemblyPath;
            launchRequest.arguments.cwd = "";
            launchRequest.arguments.console = "internalConsole";
            launchRequest.arguments.stopAtEntry = true;
            launchRequest.arguments.internalConsoleOptions = "openOnSessionStart";
            launchRequest.arguments.__sessionId = Guid.NewGuid().ToString();
            Assert.True(VSCodeDebugger.Request(launchRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void PrepareEnd(string caller_trace)
        {
            ConfigurationDoneRequest configurationDoneRequest = new ConfigurationDoneRequest();
            Assert.True(VSCodeDebugger.Request(configurationDoneRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasEntryPointHit(string caller_trace)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "entry")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasExit(string caller_trace)
        {
            bool wasExited = false;
            int ?exitCode = null;
            bool wasTerminated = false;

            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "exited")) {
                    wasExited = true;
                    ExitedEvent exitedEvent = JsonConvert.DeserializeObject<ExitedEvent>(resJSON);
                    exitCode = exitedEvent.body.exitCode;
                }
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "terminated")) {
                    wasTerminated = true;
                }
                if (wasExited && exitCode == 0 && wasTerminated)
                    return true;

                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter),
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void DebuggerExit(string caller_trace)
        {
            DisconnectRequest disconnectRequest = new DisconnectRequest();
            disconnectRequest.arguments = new DisconnectArguments();
            disconnectRequest.arguments.restart = false;
            Assert.True(VSCodeDebugger.Request(disconnectRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void AddBreakpoint(string caller_trace, string bpName, string Condition = null)
        {
            Breakpoint bp = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, bp.Type,
                         @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)bp;

            BreakpointSourceName = lbp.FileName;
            BreakpointList.Add(new SourceBreakpoint(lbp.NumLine, Condition));
            BreakpointLines.Add(lbp.NumLine);
        }

        public void SetBreakpoints(string caller_trace)
        {
            SetBreakpointsRequest setBreakpointsRequest = new SetBreakpointsRequest();
            setBreakpointsRequest.arguments.source.name = BreakpointSourceName;
            // NOTE this code works only with one source file
            setBreakpointsRequest.arguments.source.path = ControlInfo.SourceFilesPath;
            setBreakpointsRequest.arguments.lines.AddRange(BreakpointLines);
            setBreakpointsRequest.arguments.breakpoints.AddRange(BreakpointList);
            setBreakpointsRequest.arguments.sourceModified = false;
            Assert.True(VSCodeDebugger.Request(setBreakpointsRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void WasBreakpointHit(string caller_trace, string bpName)
        {
            Func<string, bool> filter = (resJSON) => {
                if (VSCodeDebugger.isResponseContainProperty(resJSON, "event", "stopped")
                    && VSCodeDebugger.isResponseContainProperty(resJSON, "reason", "breakpoint")) {
                    threadId = Convert.ToInt32(VSCodeDebugger.GetResponsePropertyValue(resJSON, "threadId"));
                    return true;
                }
                return false;
            };

            Assert.True(VSCodeDebugger.IsEventReceived(filter), @"__FILE__:__LINE__"+"\n"+caller_trace);

            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
                return;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void Continue(string caller_trace)
        {
            ContinueRequest continueRequest = new ContinueRequest();
            continueRequest.arguments.threadId = threadId;
            Assert.True(VSCodeDebugger.Request(continueRequest).Success,
                        @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public Context(ControlInfo controlInfo, NetcoreDbgTestCore.DebuggerClient debuggerClient)
        {
            ControlInfo = controlInfo;
            VSCodeDebugger = new VSCodeDebugger(debuggerClient);
        }

        public Int64 DetectFrameId(string caller_trace, string bpName)
        {
            StackTraceRequest stackTraceRequest = new StackTraceRequest();
            stackTraceRequest.arguments.threadId = threadId;
            stackTraceRequest.arguments.startFrame = 0;
            stackTraceRequest.arguments.levels = 20;
            var ret = VSCodeDebugger.Request(stackTraceRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            Breakpoint breakpoint = ControlInfo.Breakpoints[bpName];
            Assert.Equal(BreakpointType.Line, breakpoint.Type, @"__FILE__:__LINE__"+"\n"+caller_trace);
            var lbp = (LineBreakpoint)breakpoint;

            StackTraceResponse stackTraceResponse =
                JsonConvert.DeserializeObject<StackTraceResponse>(ret.ResponseStr);

            if (stackTraceResponse.body.stackFrames[0].line == lbp.NumLine
                && stackTraceResponse.body.stackFrames[0].source.name == lbp.FileName
                // NOTE this code works only with one source file
                && stackTraceResponse.body.stackFrames[0].source.path == ControlInfo.SourceFilesPath)
                return stackTraceResponse.body.stackFrames[0].id;

            throw new ResultNotSuccessException(@"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetAndCheckValue(string caller_trace, Int64 frameId, string ExpectedResult, string ExpectedType, string Expression)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.Equal(ExpectedResult, evaluateResponse.body.result, @"__FILE__:__LINE__"+"\n"+caller_trace);
            Assert.Equal(ExpectedType, evaluateResponse.body.type, @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void CheckErrorAtRequest(string caller_trace, Int64 frameId, string Expression, string errMsgStart)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = Expression;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.False(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);

            EvaluateResponse evaluateResponse =
                JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);

            Assert.True(evaluateResponse.message.StartsWith(errMsgStart), @"__FILE__:__LINE__"+"\n"+caller_trace);
        }

        public void GetResultAsString(string caller_trace, Int64 frameId, string expr, out string strRes)
        {
            EvaluateRequest evaluateRequest = new EvaluateRequest();
            evaluateRequest.arguments.expression = expr;
            evaluateRequest.arguments.frameId = frameId;
            var ret = VSCodeDebugger.Request(evaluateRequest);
            Assert.True(ret.Success, @"__FILE__:__LINE__"+"\n"+caller_trace);
            EvaluateResponse evaluateResponse = JsonConvert.DeserializeObject<EvaluateResponse>(ret.ResponseStr);
            strRes = evaluateResponse.body.result;
        }

        ControlInfo ControlInfo;
        VSCodeDebugger VSCodeDebugger;
        int threadId = -1;
        string BreakpointSourceName;
        List<SourceBreakpoint> BreakpointList = new List<SourceBreakpoint>();
        List<int> BreakpointLines = new List<int>();
    }
}

namespace VSCodeTestEvalArraysIndexers
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
            // first checkpoint (initialization) must provide "init" as id
            Label.Checkpoint("init", "bp_test", (Object context) => {
                Context Context = (Context)context;
                Context.PrepareStart(@"__FILE__:__LINE__");
                Context.AddBreakpoint(@"__FILE__:__LINE__", "BREAK1");
                Context.SetBreakpoints(@"__FILE__:__LINE__");
                Context.PrepareEnd(@"__FILE__:__LINE__");
                Context.WasEntryPointHit(@"__FILE__:__LINE__");
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

            SimpleInt sinull;
            SimpleInt? siq;

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

            Label.Checkpoint("bp_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "BREAK1");
                Int64 frameId = Context.DetectFrameId(@"__FILE__:__LINE__", "BREAK1");
                // check simpleInt indexer
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{int[2, 3]}", "int[,]", "multiArray");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multiArray[0,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multiArray[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "104", "int", "multiArray[1,0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "102", "int", "multiArray[0,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "104", "int", "multiArray[i1,i0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "102", "int", "multiArray[i0,i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "101", "int", "multiArray[ 0 , 0 ]"); // check spaces
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "105", "int", "multiArray  [ 1,1 ]"); // check spaces

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "simpleInt[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "simpleInt[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "22", "int", "simpleInt[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "333", "int", "simpleInt[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "simpleInt[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "simpleInt[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "simpleInt[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "simpleInt[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "simpleInt[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "555", "int", "simpleInt[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "66", "int", "simpleInt[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "simpleInt[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "simpleInt[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "88", "int", "simpleInt[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "99", "int", "simpleInt[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "simpleInt[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "simpleInt[i11]");

                // check twoDimInt (child of simpleInt) indexer
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "twoDimInt[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "twoDimInt[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "22", "int", "twoDimInt[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "33", "int", "twoDimInt[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "twoDimInt[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "twoDimInt[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "twoDimInt[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "twoDimInt[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "twoDimInt[ i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "twoDimInt[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "twoDimInt[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "66", "int", "twoDimInt[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "twoDimInt[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "twoDimInt[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "88", "int", "twoDimInt[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "99", "int", "twoDimInt[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i11]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[ 1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[1,1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[1, 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[ 1, 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "111", "int", "twoDimInt[ 1 , 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "222", "int", "twoDimInt[2,2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "444", "int", "twoDimInt[4,4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "9", "int", "twoDimInt[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "19", "int", "twoDimInt[i1+i2,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i1+i4,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "twoDimInt[i1, i2+i4]");

                //check indexAsString
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "0", "int", "indexAsString[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[ 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[ 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[ i1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "11", "int", "indexAsString[ i1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "22", "int", "indexAsString[2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "33", "int", "indexAsString[3]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "indexAsString[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "44", "int", "indexAsString[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "indexAsString[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "indexAsString[i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "indexAsString[ i1+i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "indexAsString[i1 + i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "55", "int", "indexAsString[ i1 + i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "66", "int", "indexAsString[6]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "indexAsString[7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "77", "int", "indexAsString[i7]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "88", "int", "indexAsString[8]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "99", "int", "indexAsString[9]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[11]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i11]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[ 1,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[1,1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[1, 1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[ 1, 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "6", "int", "indexAsString[ 1 , 1 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "12", "int", "indexAsString[2,2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "24", "int", "indexAsString[4,4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "9", "int", "indexAsString[i1,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "19", "int", "indexAsString[i1+i2,i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i1+i4,1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.IndexOutOfRangeException}", "System.IndexOutOfRangeException", "indexAsString[i1, i2+i4]");

                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "indexAsString[1,str[1]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "indexAsString[2, str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "9", "int", "indexAsString[3,str[3] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "16", "int", "indexAsString[4, str[4]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "25", "int", "indexAsString[ 5, str[5] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "36", "int", "indexAsString[ 6 , str[6] ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "indexAsString[11,str[10]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "indexAsString[i1, str[10]]");

                // check Dictionary<int,string>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"one\"", "string", "dictis[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[ 4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[ 4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[ i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictis[ i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[ i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[ i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[ i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[i1 + i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "dictis[ i1 + i2 ]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictis[\"four\"]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictis[str[4]]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictis[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictis[i7]");
 
                // check Dictionary<string, int>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "dictsi[\"one\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "dictsi[ \"one\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "dictsi[\"one\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "dictsi[ \"one\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[\"four\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[ \"four\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[\"four\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[ \"four\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "dictsi[str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "dictsi[str[ 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "dictsi[str[2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "dictsi[str[ 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[ 2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[ 2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[ 2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[2 + 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictsi[str[ 2 + 2 ]]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictsi[4]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictsi[i4]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictsi[str[5]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictsi[\"five\"]");

                // check List<string>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"one\"", "string", "lists[1]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[ 4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[ 4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[ i4]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "lists[ i4 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[ i1+i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[ i1+i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[ i1 + i2]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[i1 + i2 ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"three\"", "string", "lists[ i1 + i2 ]");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists[4.4]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists[str[4]]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "lists[\"four\"]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "lists[5]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "lists[i7]");

                // check List<MyString>
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{VSCodeTestEvalArraysIndexers.MyString}", "VSCodeTestEvalArraysIndexers.MyString", "listms[3]");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{VSCodeTestEvalArraysIndexers.MyString}", "VSCodeTestEvalArraysIndexers.MyString", "listms[ 3]");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{VSCodeTestEvalArraysIndexers.MyString}", "VSCodeTestEvalArraysIndexers.MyString", "listms[3 ]");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{VSCodeTestEvalArraysIndexers.MyString}", "VSCodeTestEvalArraysIndexers.MyString", "listms[ 3 ]");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"one\"", "string", "listms[1].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[4].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[ 4].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[4 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[ 4 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[i4].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[ i4].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[i4 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"four\"", "string", "listms[ i4 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[i1+i2].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[ i1+i2].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[i1+i2 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[ i1+i2 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[i1 + i2].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[ i1 + i2].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[i1 + i2 ].s");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "\"three\"", "string", "listms[ i1 + i2 ].s");
                Context.CheckErrorAtRequest(@"__FILE__ :__LINE__", frameId, "listms[4.4]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__ :__LINE__", frameId, "listms[myStrings[4]]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "listms[5]");
                Context.GetAndCheckValue(@"__FILE__ :__LINE__", frameId, "{System.ArgumentOutOfRangeException}", "System.ArgumentOutOfRangeException", "listms[i7]");

                // check SortedList<string,int>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "slist[\"one\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "slist[ \"one\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "slist[\"one\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "slist[ \"one\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[\"four\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[ \"four\"]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[\"four\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[ \"four\" ]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "slist[str[2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "slist[str[ 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "slist[str[2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "2", "int", "slist[str[ 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[ 2+2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[ 2+2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[ 2 + 2]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[2 + 2 ]]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "slist[str[ 2 + 2 ]]");

                // check Dictionary<MyInt,MyString>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"one\"", "string", "dictmims[myInts[1]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictmims[myInts[4]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictmims[ myInts[4]].s");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictmims[myInts[4]].s ");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "\"four\"", "string", "dictmims[ myInts[4]].s ");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictmims[myStrings[4]]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictmims[\"a string\"]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictmims[myInts[5]]");

                // check Dictionary<MyInt,MyString>
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "1", "int", "dictmsmi[myStrings[1]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictmsmi[myStrings[4]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictmsmi[ myStrings[4]].i");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictmsmi[myStrings[4]].i ");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "4", "int", "dictmsmi[ myStrings[4]].i ");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictmsmi[myInts[4]]", "error: 0x80070057");
                Context.CheckErrorAtRequest(@"__FILE__:__LINE__", frameId, "dictmsmi[\"a string\"]", "error: 0x80070057");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.Collections.Generic.KeyNotFoundException}", "System.Collections.Generic.KeyNotFoundException", "dictmsmi[myStrings[5]]");

                // check nullables
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvalArraysIndexers.SimpleInt", "sinull");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.NullReferenceException}", "System.NullReferenceException", "sinull[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvalArraysIndexers.SimpleInt", "sinull?[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvalArraysIndexers.SimpleInt", "siq");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "{System.NullReferenceException}", "System.NullReferenceException", "siq[0]");
                Context.GetAndCheckValue(@"__FILE__:__LINE__", frameId, "null", "VSCodeTestEvalArraysIndexers.SimpleInt", "siq?[0]");

                Context.Continue(@"__FILE__:__LINE__");
            });

            // last checkpoint must provide "finish" as id or empty string ("") as next checkpoint id
            Label.Checkpoint("finish", "", (Object context) => {
                Context Context = (Context)context;
                Context.WasExit(@"__FILE__:__LINE__");
                Context.DebuggerExit(@"__FILE__:__LINE__");
            });
        }
    }
}
