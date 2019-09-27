using System;
using System.IO;
using System.Collections.Generic;
using System.Threading;

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
            DebuggerInput.Write(CONTENT_LENGTH + command.Length.ToString() + TWO_CRLF + command);
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

            while (true) {
                // Read until "\r\n\r\n"
                int res = DebuggerOutput.Read();
                if (res < 0) {
                    return null;
                }

                header += (char)res;

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

                char[] buffer = new char[contentLength + 1];
                buffer[contentLength] = '\0';
                int buffer_i = 0;
                while (buffer_i < contentLength) {
                    int count = 0;
                    try {
                        count = DebuggerOutput.Read(buffer, buffer_i, contentLength - buffer_i);
                    }
                    catch (IOException) {
                        return null;
                    }
                    buffer_i += count;
                }

                return new string(buffer);
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
                throw new DebuggerNotResponsesException();
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
