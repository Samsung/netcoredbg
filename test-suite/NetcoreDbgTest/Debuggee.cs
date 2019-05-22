using System;
using System.Collections.Generic;

using NetcoreDbgTest;

namespace NetcoreDbgTestCore
{
    public static class Debuggee
    {
        public static DebuggerClient DebuggerClient = null;
        public static Dictionary<string, Breakpoint> Breakpoints = null;
        public static string TestName = null;
        public static string SourceFilesPath = null;
        public static string TargetAssemblyPath = null;
        public static string CorerunPath = null;

        public static void Invoke(string id, string next_id, Checkpoint checkpoint)
        {
            checkpoint();
        }

        public static void Run(DebuggeeScript script,
                               DebuggerClient debugger,
                               NetcoreDbgTestCore.Environment env)
        {
            DebuggerClient = debugger;
            Breakpoints = script.Breakpoints;
            TestName = env.TestName;
            SourceFilesPath = env.SourceFilesPath;
            TargetAssemblyPath = env.TargetAssemblyPath;
            CorerunPath = env.CorerunPath;

            script.ExecuteCheckPoints();

            DebuggerClient = null;
            Breakpoints = null;
            TestName = null;
            SourceFilesPath = null;
            TargetAssemblyPath = null;
            CorerunPath = null;
        }
    }

    public class Environment
    {
        public string TestName = null;
        public string SourceFilesPath = null;
        public string TargetAssemblyPath = null;
        public string CorerunPath = "dotnet";
    }
}
