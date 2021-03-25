using System.IO;
using System.Collections.Generic;
using System.Threading;

using NetcoreDbgTestCore;

namespace NetcoreDbgTestCore.MI
{
    public class MILocalDebuggerClient : DebuggerClient
    {
        public MILocalDebuggerClient(StreamWriter input, StreamReader output)
            : base(ProtocolType.MI)
        {
            DebuggerInput = input;
            DebuggerOutput = output;
            GetInput = new AutoResetEvent(false);
            GotInput = new AutoResetEvent(false);
            InputThread = new Thread(ReaderThread);
            InputThread.IsBackground = true;
            InputThread.Start();
        }

        public override bool DoHandshake(int timeout)
        {
            string[] output = ReceiveOutputLines(timeout);

            return output != null && output.Length == 1;
        }

        public override bool Send(string command)
        {
            DebuggerInput.WriteLine(command);

            return true;
        }

        public override string[] Receive(int timeout)
        {
            return ReceiveOutputLines(timeout);
        }

        public override void Close()
        {
            DebuggerInput.Close();
            DebuggerOutput.Close();
        }

        void ReaderThread()
        {
            while (true) {
                GetInput.WaitOne();
                InputString = DebuggerOutput.ReadLine();
                GotInput.Set();
            }
        }

        string[] ReceiveOutputLines(int timeout)
        {
            var output = new List<string>();

            while (true) {
                GetInput.Set();
                bool success = GotInput.WaitOne(timeout);
                if (!success)
                    throw new DebuggerNotResponses();

                if (InputString == null) {
                    return null;
                }

                output.Add(InputString);

                if (InputString == "(gdb)") {
                    break;
                }
            }

            return output.ToArray();
        }

        StreamWriter DebuggerInput;
        StreamReader DebuggerOutput;
        Thread InputThread;
        AutoResetEvent GetInput, GotInput;
        string InputString;
    }
}
