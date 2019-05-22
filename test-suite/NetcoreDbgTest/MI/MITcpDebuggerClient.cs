using System.Net.Sockets;
using System.Text;

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
                PendingOutput = "";
            }

            public override bool DoHandshake(int timeout)
            {
                string[] output = ReceiveOutputLines(timeout);

                // output must consist of one line "(gdb)"
                return output != null && output.Length == 1;
            }

            public override bool Send(string cmd)
            {
                SendCommandLine(cmd);

                return true;
            }

            public override string[] Receive(int timeout)
            {
                return ReceiveOutputLines(timeout);
            }

            public override void Close()
            {
                stream.Close();
                client.Close();
            }

            void SendCommandLine(string str)
            {
                byte[] bytes = Encoding.ASCII.GetBytes(str + "\n");
                stream.Write(bytes, 0, bytes.Length);
            }

            string[] ReceiveOutputLines(int timeout)
            {
                int lenOfGdb = "(gdb)".Length;
                var sb = new StringBuilder(PendingOutput);
                int indexOfGdb = PendingOutput.IndexOf("(gdb)");

                while (indexOfGdb == -1) {
                    string availableData = LoadAvailableData(timeout);

                    if (availableData == null) {
                        return null;
                    }

                    sb.Append(availableData);
                    PendingOutput = sb.ToString();
                    indexOfGdb = PendingOutput.IndexOf("(gdb)");
                }

                var packedOutputLines = PendingOutput.Substring(0, indexOfGdb + lenOfGdb);
                PendingOutput = PendingOutput.Substring(indexOfGdb + lenOfGdb + 1);

                return packedOutputLines.TrimEnd('\r', '\n').Split("\n");
            }

            string LoadAvailableData(int timeout)
            {
                byte[] recvBuffer = new byte[64];
                StringBuilder sb = new StringBuilder();
                int recvCount = 0;
                string response;

                stream.ReadTimeout = timeout;

                try {
                    do {
                        int readCount = stream.Read(recvBuffer, 0, recvBuffer.Length);
                        response = Encoding.UTF8.GetString(recvBuffer, 0, readCount);
                        sb.Append(response);
                        recvCount += readCount;
                    } while (stream.DataAvailable);
                }

                catch {
                }

                stream.ReadTimeout = -1;

                if (recvCount == 0) {
                    return null;
                }

                return sb.ToString();
            }

            TcpClient client;
            NetworkStream stream;
            string PendingOutput;
        }
    }
}
