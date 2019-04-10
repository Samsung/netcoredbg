using System.IO;
using System.Diagnostics;

namespace LocalDebugger
{
    public class LocalDebuggerProcess
    {
        public LocalDebuggerProcess(string debuggerPath, string debuggerArg)
        {
            DebuggerProcess = new Process();
            DebuggerProcess.StartInfo.FileName = debuggerPath;
            DebuggerProcess.StartInfo.Arguments = debuggerArg;
            DebuggerProcess.StartInfo.UseShellExecute = false;
            DebuggerProcess.StartInfo.RedirectStandardInput = true;
            DebuggerProcess.StartInfo.RedirectStandardOutput = true;
            Input = null;
            Output = null;
        }

        public void Start()
        {
            DebuggerProcess.Start();
            Input = DebuggerProcess.StandardInput;
            Output = DebuggerProcess.StandardOutput;
        }

        public void Close()
        {
            DebuggerProcess.Kill();
            DebuggerProcess.WaitForExit();
            DebuggerProcess.Dispose();
        }

        public StreamWriter Input;
        public StreamReader Output;
        public Process DebuggerProcess;
    }
}
