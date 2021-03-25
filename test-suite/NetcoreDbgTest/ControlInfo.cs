using System.Collections.Generic;
using NetcoreDbgTestCore;

namespace NetcoreDbgTest
{
    public class ControlInfo
    {
        public System.Collections.Generic.Dictionary<string, Breakpoint> Breakpoints { get; private set; }
        public string TestName { get; private set; }
        public string SourceFilesPath { get; private set; }
        public string TargetAssemblyPath { get; private set; }
        public string CorerunPath { get; private set; }

        public ControlInfo(ControlScript script,
                           NetcoreDbgTestCore.Environment env)
        {
            Breakpoints = script.Breakpoints;
            TestName = env.TestName;
            SourceFilesPath = env.SourceFilesPath;
            TargetAssemblyPath = env.TargetAssemblyPath;
            CorerunPath = env.CorerunPath;
        }
    }
}
