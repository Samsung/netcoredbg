namespace NetcoreDbgTestCore
{
    public class Exception : System.Exception
    {
    }

    public class ResultNotSuccessException : Exception
    {
    }

    public class DebuggerNotResponsesException : Exception
    {
    }

    public class DebuggerTimedOut : Exception
    {
    }
}
