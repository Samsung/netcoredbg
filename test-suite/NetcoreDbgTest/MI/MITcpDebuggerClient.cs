using System.Net.Sockets;
using System.Text;
using System.IO;
using System.Collections.Generic;

namespace NetcoreDbgTestCore
{
    namespace MI
    {
        public class MITcpDebuggerClient : DebuggerClient
        {
            public MITcpDebuggerClient(string addr, int port) : base(ProtocolType.MI)
            {
                client = new TcpClient(addr, port);
                stream = client.GetStream();
                DebuggerInput = new StreamWriter(stream);
                DebuggerOutput = new StreamReader(stream);
            }

            public override bool DoHandshake(int timeout)
            {
                string[] output = ReceiveOutputLines(timeout);

                // output must consist of one line "(gdb)"
                return output != null && output.Length == 1;
            }

            public override bool Send(string cmd)
            {
                DebuggerInput.WriteLine(cmd);
                DebuggerInput.Flush();

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
                stream.Close();
                client.Close();
            }

            string[] ReceiveOutputLines(int timeout)
            {
                var output = new List<string>();
                stream.ReadTimeout = timeout;

                while (true) {
                    string InputString = DebuggerOutput.ReadLine();

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

            TcpClient client;
            NetworkStream stream;
            StreamWriter DebuggerInput;
            StreamReader DebuggerOutput;
        }
    }
}
