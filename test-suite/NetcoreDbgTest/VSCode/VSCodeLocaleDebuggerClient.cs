using System;
using System.IO;
using System.Collections.Generic;
using System.Threading;
using System.Text;

using NetcoreDbgTestCore;

namespace NetcoreDbgTestCore.VSCode
{
    public class VSCodeLocalDebuggerClient : DebuggerClient
    {
        public VSCodeLocalDebuggerClient(StreamWriter input, StreamReader output)
            : base(ProtocolType.VSCode)
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
            return true;
        }

        public override bool Send(string command)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(command);
            string commandSize = bytes.Length.ToString();
            DebuggerInput.Write(CONTENT_LENGTH + commandSize + TWO_CRLF + command);
            DebuggerInput.Flush();

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
            DebuggerInput.Close();
            DebuggerOutput.Close();
        }

        string ReadData()
        {
            string header = "";
            byte[] recvBuffer = new byte[1];

            while (true) {
                // Read until "\r\n\r\n"
                int readCount = DebuggerOutput.BaseStream.Read(recvBuffer, 0, recvBuffer.Length);
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
                        count = DebuggerOutput.BaseStream.Read(buffer, buffer_i, contentLength - buffer_i);
                    }
                    catch (SystemException ex) when (ex is InvalidOperationException ||
                                                     ex is IOException ||
                                                     ex is ObjectDisposedException) {
                        return null;
                    }
                    buffer_i += count;
                }

                return Encoding.UTF8.GetString(buffer);
            }
            // unreachable
        }

        void ReaderThread()
        {
            while (true) {
                GetInput.WaitOne();
                InputString = ReadData();
                GotInput.Set();
            }
        }

        string ReceiveOutputLine(int timeout)
        {
            GetInput.Set();
            bool success = GotInput.WaitOne(timeout);
            if (!success) {
                throw new DebuggerNotResponses();
            }

            if (InputString == null) {
                return null;
            }

            return InputString;
        }

        StreamWriter DebuggerInput;
        StreamReader DebuggerOutput;
        Thread InputThread;
        AutoResetEvent GetInput, GotInput;
        string InputString;
        static string TWO_CRLF = "\r\n\r\n";
        static string CONTENT_LENGTH = "Content-Length: ";
    }
}
