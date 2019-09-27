using System;
using System.IO;
using System.Net.Sockets;
using System.Text;

namespace NetcoreDbgTestCore
{
    namespace VSCode
    {
        public class VSCodeTcpDebuggerClient : DebuggerClient
        {
            public VSCodeTcpDebuggerClient(string addr, int port) : base(ProtocolType.VSCode)
            {
                client = new TcpClient(addr, port);

                stream = client.GetStream();
            }

            public override bool DoHandshake(int timeout)
            {
                return true;
            }

            public override bool Send(string cmd)
            {
                SendCommandLine(CONTENT_LENGTH + cmd.Length.ToString() + TWO_CRLF + cmd);

                return true;
            }

            public override string[] Receive(int timeout)
            {
                string line = ReceiveOutputLine(timeout);
                if (line == null) {
                    return null;
                }
                return new string[1]{line};
            }

            public override void Close()
            {
                stream.Close();
                client.Close();
            }

            void SendCommandLine(string str)
            {
                byte[] bytes = Encoding.UTF8.GetBytes(str);
                stream.Write(bytes, 0, bytes.Length);
            }

            string ReceiveOutputLine(int timeout)
            {
                string header = "";
                byte[] recvBuffer = new byte[1];

                while (true) {
                    // Read until "\r\n\r\n"
                    int readCount = stream.Read(recvBuffer, 0, recvBuffer.Length);
                    header += Encoding.ASCII.GetString(recvBuffer, 0, readCount);

                    if (header.Length < TWO_CRLF.Length) {
                       continue;
                    }

                    if (header.Substring(header.Length - TWO_CRLF.Length, TWO_CRLF.Length) != TWO_CRLF) {
                        continue;
                    }

                    // Extract Content-Length
                    int lengthIndex = header.IndexOf(CONTENT_LENGTH);
                    if (lengthIndex == -1) {
                        continue;
                    }

                    int contentLength = Int32.Parse(header.Substring(lengthIndex + CONTENT_LENGTH.Length));

                    byte[] buffer = new byte[contentLength + 1];
                    buffer[contentLength] = 0;
                    int buffer_i = 0;
                    while (buffer_i < contentLength) {
                        int count = 0;
                        try {
                            count = stream.Read(buffer, buffer_i, contentLength - buffer_i);
                        }
                        catch (SystemException ex) when (ex is InvalidOperationException ||
                                                         ex is IOException ||
                                                         ex is ObjectDisposedException) {
                            return null;
                        }
                        buffer_i += count;
                    }

                    return System.Text.Encoding.UTF8.GetString(buffer);
                }
                // unreachable
            }

            TcpClient client;
            NetworkStream stream;
            static string TWO_CRLF = "\r\n\r\n";
            static string CONTENT_LENGTH = "Content-Length: ";
        }
    }
}
