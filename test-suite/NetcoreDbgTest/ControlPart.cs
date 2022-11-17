using System;

using NetcoreDbgTest;

namespace NetcoreDbgTestCore
{
    public class ControlPart
    {
        public void Run(ControlScript script,
                        DebuggerClient debugger,
                        NetcoreDbgTestCore.Environment env)
        {
            ControlInfo info = new ControlInfo(script, env);
            script.ExecuteCheckPoints(info, debugger);
        }
    }

    public class Environment
    {
        public string TestName = null;
        public string SourceFilesPath = null;
        public string TargetAssemblyPath = null;
        public string CorerunPath = "dotnet";
        public string SDB = "sdb";
    }
}
