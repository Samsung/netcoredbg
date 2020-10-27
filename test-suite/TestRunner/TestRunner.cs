using System;
using System.IO;
using System.Net;
using System.Collections.Generic;
using System.Diagnostics;

using LocalDebugger;
using NetcoreDbgTestCore;
using NetcoreDbgTestCore.MI;
using NetcoreDbgTestCore.VSCode;

namespace TestRunner
{
    class Program
    {
        public static int Main(string[] args)
        {
            var cli = new CLInterface(args);
            DebuggerClient debugger = null;
            DebuggeeScript script = null;
            LocalDebuggerProcess localDebugger = null;

            if (cli.NeedHelp) {
                cli.PrintHelp();
                return 1;
            }

            if (cli.ClientInfo == null) {
                Console.Error.WriteLine("Please define client type");
                return 1;
            }

            try {
                switch (cli.Protocol) {
                case ProtocolType.MI:
                    switch (cli.ClientInfo.Type) {
                    case ClientType.Local:
                        var localClientInfo = (LocalClientInfo)cli.ClientInfo;
                        localDebugger = new LocalDebuggerProcess(
                                            localClientInfo.DebuggerPath, @" --interpreter=mi");
                        localDebugger.Start();

                        debugger = new MILocalDebuggerClient(localDebugger.Input,
                                                             localDebugger.Output);
                        break;
                    case ClientType.Tcp:
                        var tcpClientInfo = (TcpClientInfo)cli.ClientInfo;
                        debugger = new MITcpDebuggerClient(tcpClientInfo.Addr,
                                                           tcpClientInfo.Port);
                        break;
                    default:
                            Console.Error.WriteLine("Only tcp and local debuggers are supported now");
                            return 1;
                    }

                    break;

                case ProtocolType.VSCode:
                    switch (cli.ClientInfo.Type) {
                    case ClientType.Local:
                        var localClientInfo = (LocalClientInfo)cli.ClientInfo;
                        localDebugger = new LocalDebuggerProcess(
                                            localClientInfo.DebuggerPath, @" --interpreter=vscode");
                        localDebugger.Start();

                        debugger = new VSCodeLocalDebuggerClient(localDebugger.Input,
                                                                 localDebugger.Output);
                        break;
                    case ClientType.Tcp:
                        var tcpClientInfo = (TcpClientInfo)cli.ClientInfo;
                        debugger = new VSCodeTcpDebuggerClient(tcpClientInfo.Addr,
                                                               tcpClientInfo.Port);
                        break;
                    default:
                            Console.Error.WriteLine("Only tcp and local debuggers are supported now");
                            return 1;
                    }

                    break;

                default:
                    Console.Error.WriteLine("Only GDB/MI and VSCode protocols is supported now");
                    return 1;
                }
            }
            catch {
                Console.Error.WriteLine("Can't create debugger client");
                if (localDebugger != null) {
                    localDebugger.Close();
                }
                return 1;
            }

            if (!debugger.DoHandshake(5000)) {
                Console.Error.WriteLine("Handshake is failed");
                debugger.Close();
                if (localDebugger != null) {
                    localDebugger.Close();
                }
                return 1;
            }

            try {
                script = new DebuggeeScript(cli.Environment.SourceFilesPath, debugger.Protocol);
            }
            catch (ScriptNotBuiltException e) {
                Console.Error.WriteLine("Script is not built:");
                Console.Error.WriteLine(e.ToString());
                debugger.Close();
                if (localDebugger != null) {
                    localDebugger.Close();
                }
                return 1;
            }

            try {
                Debuggee.Run(script, debugger, cli.Environment);
                Console.WriteLine("Success: Test case \"{0}\" is passed!!!",
                                  cli.Environment.TestName);
            }
            catch (System.Exception e) {
                Console.Error.WriteLine("Script running is failed. Got exception:\n" + e.ToString());
                debugger.Close();
                if (localDebugger != null) {
                    localDebugger.Close();
                }
                return 1;
            }

            debugger.Close();
            if (localDebugger != null) {
                localDebugger.Close();
            }
            return 0;
        }
    }

    enum ClientType
    {
        Local,
        Tcp,
    }

    class CLInterface
    {
        public CLInterface(string[] args)
        {
            Environment = new NetcoreDbgTestCore.Environment();

            if (args.Length == 0) {
                NeedHelp = true;
                return;
            }

            if (args.Length == 1 && (args[0] == "-h" || args[0] == "--help")) {
                NeedHelp = true;
                return;
            }

            int i = 0;
            while (i < args.Length && !NeedHelp) {
                switch (args[i]) {
                case "--tcp":
                    if (i + 2 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    try {
                        ClientInfo = new TcpClientInfo(args[i + 1],
                                                       args[i + 2]);
                    }
                    catch {
                        NeedHelp = true;
                        break;
                    }
                    i += 3;

                    break;
                case "--local":
                    if (i + 1 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    try {
                        string debuggerPath = Path.GetFullPath(args[i + 1]);
                        ClientInfo = new LocalClientInfo(debuggerPath);
                    }
                    catch {
                        NeedHelp = true;
                        break;
                    }
                    i += 2;

                    break;
                case "--proto":
                    if (i + 1 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    switch (args[i + 1]) {
                    case "mi":
                        Protocol = ProtocolType.MI;
                        break;
                    case "vscode":
                        Protocol = ProtocolType.VSCode;
                        break;
                    default:
                        Protocol = ProtocolType.None;
                        break;
                    }
                    i += 2;

                    break;
                case "--test":
                    if (i + 2 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    try {
                        Environment.TestName = args[i + 1];
                    }
                    catch {
                        NeedHelp = true;
                        break;
                    }

                    i += 2;

                    break;
                case "--sources":
                    if (i + 1 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    try {
                        Environment.SourceFilesPath = Path.GetFullPath(args[i + 1]);
                        if (Environment.SourceFilesPath[Environment.SourceFilesPath.Length - 1] == ';') {
                            Environment.SourceFilesPath =
                                Environment.SourceFilesPath.Remove(Environment.SourceFilesPath.Length - 1);
                        }
                    }
                    catch {
                        NeedHelp = true;
                        break;
                    }

                    i += 2;

                    break;
                case "--assembly":
                    if (i + 1 >= args.Length) {
                        NeedHelp = true;
                        break;
                    }

                    Environment.TargetAssemblyPath = args[i + 1];

                    i += 2;

                    break;
                case "--dotnet":
                    if (i + 1 >= args.Length) {
                        NeedHelp = true;
                    }

                    try {
                        Path.GetFullPath(args[i + 1]);
                    }
                    catch {
                        NeedHelp = true;
                        break;
                    }

                    Environment.CorerunPath = args[i + 1];

                    i += 2;

                    break;
                default:
                    NeedHelp = true;
                    break;
                }
            }

            if (ClientInfo != null
                && ClientInfo.Type == ClientType.Local
                && !File.Exists(Environment.TargetAssemblyPath)) {
                Console.Error.WriteLine("Provided assembly path is invalid");
                throw new System.Exception();
            }

            if (NeedHelp) {
                return;
            }
        }

        public void PrintHelp()
        {
            Console.Error.WriteLine(
@"usage: dotnet run {-h|--help|[OPTIONS] TESTS}
options:
    --dotnet dotnet-path    Set dotnet path(default: dotnet-path=""dotnet"")
    --proto protocol        Set protocol(default: protocol=mi)
    --tcp server port       Create TCP client for debugger
    --local debugger-path   Create launch debugger locally and create client
    --test name             Test name
    --sources path[;path]   Semicolon separated paths to source files
    --assembly path         Path to target assambly file
    
    
    ");
        }

        public bool NeedHelp = false;
        public ProtocolType Protocol = ProtocolType.MI;
        public NetcoreDbgTestCore.Environment Environment;
        public ClientInfo ClientInfo;
    }

    class ClientInfo
    {
        public ClientType Type;
    }

    class TcpClientInfo : ClientInfo
    {
        public TcpClientInfo(string addr, string port)
        {
            Type = ClientType.Tcp;

            if (!IsIpAddressValid(addr)) {
                Console.Error.WriteLine("IP address is invalid");
                throw new System.Exception();
            }

            Addr = addr;
            Port = Int32.Parse(port);
        }

        private bool IsIpAddressValid(string addr)
        {
            try {
                IPAddress[] hostIPs = Dns.GetHostAddresses(addr);
                IPAddress[] localIPs = Dns.GetHostAddresses(Dns.GetHostName());

                foreach (IPAddress hostIP in hostIPs) {
                    if (IPAddress.IsLoopback(hostIP)) {
                        return true;
                    }
                    foreach (IPAddress localIP in localIPs) {
                        if (hostIP.Equals(localIP)) {
                            return true;
                        }
                    }
                }

                IPAddress address = IPAddress.Parse(addr);
                return true;
            }
            catch {
            }

            return false;
        }
        public string Addr;
        public int Port;
    }

    class LocalClientInfo : ClientInfo
    {
        public LocalClientInfo(string debuggerPath)
        {
            Type = ClientType.Local;
            DebuggerPath = debuggerPath;
        }

        public string DebuggerPath;
    }
}
