namespace NetcoreDbgTestCore
{
    public class BrokenTestCheckpointLogic : System.Exception
    {

    }

    public class DebuggerNotResponses : System.Exception
    {
    }

    public class WrongResponseSequence : System.Exception
    {
    }
}

namespace NetcoreDbgTest
{
    public class NetcoreDbgTestException : System.Exception
    {
        public NetcoreDbgTestException(string stacktrace)
        {
            UserMessage = "\n Stack Trace:";

            foreach (string line in stacktrace.Split('\n'))
            {
                UserMessage += "\n   at line " + line;
            }
        }

        string UserMessage;
        public override string Message => UserMessage;
    }

    public class DebuggerTimedOut : NetcoreDbgTestException
    {
        public DebuggerTimedOut(string stacktrace)
                : base(stacktrace)
        {}
    }

    public class ResultNotSuccessException : NetcoreDbgTestException
    {
        public ResultNotSuccessException(string stacktrace)
                : base(stacktrace)
        {}
    }

    public class ResultEqualException : System.Exception
    {
        public ResultEqualException(object expected, object actual, string stacktrace)
        {
            UserMessage = "\n Expected:  " + expected.ToString() +
                          "\n Actual:    " + actual.ToString() + 
                          "\n Stack Trace:" ;

            foreach (string line in stacktrace.Split('\n'))
            {
                UserMessage += "\n   at line " + line;
            }
        }

        string UserMessage;
        public override string Message => UserMessage;
    }

    public class ResultNotEqualException : System.Exception
    {
        public ResultNotEqualException(object expected, object actual, string stacktrace)
        {
            UserMessage = "\n Expected:  Not " + expected.ToString() +
                          "\n Actual:    " + actual.ToString() + 
                          "\n Stack Trace:";

            foreach (string line in stacktrace.Split('\n'))
            {
                UserMessage += "\n   at line " + line;
            }
        }

        string UserMessage;
        public override string Message => UserMessage;
    }

    public class ResultTrueException : ResultEqualException
    {
        public ResultTrueException(object expected, object actual, string stacktrace)
                : base(expected, actual, stacktrace)
        {}
    }

    public class ResultFalseException : ResultEqualException
    {
        public ResultFalseException(object expected, object actual, string stacktrace)
                : base(expected, actual, stacktrace)
        {}
    }

    public class ResultNotNullException : System.Exception
    {
        public ResultNotNullException(string stacktrace)
        {
            UserMessage = "Assert.NotNull() Failure" +
                          "\n Stack Trace:" ;

            foreach (string line in stacktrace.Split('\n'))
            {
                UserMessage += "\n   at line " + line;
            }
        }

        string UserMessage;
        public override string Message => UserMessage;
    }
}
