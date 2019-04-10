using NetcoreDbgTestCore;

namespace NetcoreDbgTest
{
    public static class DebuggeeInfo
    {
        public static System.Collections.Generic.Dictionary<string, Breakpoint> Breakpoints
        {
            get { return Debuggee.Breakpoints; }
        }

        public static string TestName
        {
            get { return Debuggee.TestName; }
        }

        public static string SourceFilesPath
        {
            get { return Debuggee.SourceFilesPath; }
        }

        public static string TargetAssemblyPath
        {
            get { return Debuggee.TargetAssemblyPath; }
        }

        public static string CorerunPath
        {
            get { return Debuggee.CorerunPath; }
        }
    }
}
