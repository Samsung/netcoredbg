// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "manageddebugger.h"
#include "miprotocol.h"
#include "vscodeprotocol.h"
#include "logger.h"

static const uint16_t DEFAULT_SERVER_PORT = 4711;

static void print_help()
{
    fprintf(stdout,
        ".NET Core debugger\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n"
        "--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.\n"
        "--engineLogging[=<path to log file>]  Enable logging to VsDbg-UI or file for the engine.\n"
        "                                      Only supported by the VsCode interpreter.\n"
        "--server[=port_num]                   Start the debugger listening for requests on the\n"
        "                                      specified TCP/IP port instead of stdin/out. If port is not specified\n"
        "                                      TCP %i will be used.\n"
        "--log-file[=<debug>]                  Enable logging to file which is created in 'current'. Supported\n"
        "                                      two log levels: info and debug. Info contains info, warn and error\n"
        "                                      messages, debug the same and debug messages in addition. Info level\n"
        "                                      is the default one.\n",
        (int)DEFAULT_SERVER_PORT
    );
}

int main(int argc, char *argv[])
{
    DWORD pidDebuggee = 0;

    enum InterpreterType
    {
        InterpreterMI,
        InterpreterVSCode
    } interpreterType = InterpreterMI;

    bool engineLogging = false;
    LogType logType = NO_LOG;
    LogLevel logLevel = LOG_INFO;
    std::string logFilePath;

    uint16_t serverPort = 0;

    std::string execFile;
    std::vector<std::string> execArgs;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--attach") == 0)
        {
            i++;
            if (i >= argc)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
            char *err;
            pidDebuggee = strtoul(argv[i], &err, 10);
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "--interpreter=mi") == 0)
        {
            interpreterType = InterpreterMI;
            continue;
        }
        else if (strcmp(argv[i], "--interpreter=vscode") == 0)
        {
            interpreterType = InterpreterVSCode;
            continue;
        }
        else if (strcmp(argv[i], "--engineLogging") == 0)
        {
            engineLogging = true;
            continue;
        }
        else if (strstr(argv[i], "--engineLogging=") == argv[i])
        {
            engineLogging = true;
            logFilePath = argv[i] + strlen("--engineLogging=");
            continue;
        }
        else if (strcmp(argv[i], "--help") == 0)
        {
            print_help();
            return EXIT_SUCCESS;
        }
        else if (strcmp(argv[i], "--log-file") == 0)
        {
            logType = FILE_LOG;
            continue;
        }
        else if (strstr(argv[i], "--log-file=") == argv[i])
        {
            const std::string &debug = "debug";

            logType = FILE_LOG;
            if (std::string(argv[i] + strlen("--log-file=")) == debug)
                logLevel = LOG_DEBUG;
            else
                logLevel = LOG_INFO;
        }
        else if (strcmp(argv[i], "--server") == 0)
        {
            serverPort = DEFAULT_SERVER_PORT;
            continue;
        }
        else if (strstr(argv[i], "--server=") == argv[i])
        {
            char *err;
            serverPort = static_cast<uint16_t>(strtoul(argv[i] + strlen("--server="), &err, 10));
            if (*err != 0)
            {
                fprintf(stderr, "Error: Missing process id\n");
                return EXIT_FAILURE;
            }
            continue;
        }
        else if (strcmp(argv[i], "--") == 0)
        {
            ++i;
            if (i < argc)
            {
                execFile = argv[i];
            }
            else
            {
                fprintf(stderr, "Error: Missing program argument\n");
                return EXIT_FAILURE;
            }
            for (++i; i < argc; ++i)
            {
                execArgs.push_back(argv[i]);
            }
            break;
        }
        else
        {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    Logger::setLogging(logType, logLevel);
    Logger::log("Start logging to file");

    ManagedDebugger debugger;
    std::unique_ptr<Protocol> protocol;

    switch(interpreterType)
    {
        case InterpreterMI:
        {
            Logger::log("InterpreterMI selected");
            if (engineLogging)
            {
                fprintf(stderr, "Error: Engine logging is only supported in VsCode interpreter mode.\n");
                Logger::log("Error: Engine logging is only supported in VsCode interpreter mode.");
                return EXIT_FAILURE;
            }
            MIProtocol *miProtocol = new MIProtocol();
            protocol.reset(miProtocol);
            miProtocol->SetDebugger(&debugger);
            Logger::log("SetDebugger for InterpreterMI");
            if (!execFile.empty())
                miProtocol->SetLaunchCommand(execFile, execArgs);
            break;
        }
        case InterpreterVSCode:
        {
            Logger::log("InterpreterVSCode selected");
            VSCodeProtocol *vsCodeProtocol = new VSCodeProtocol();
            protocol.reset(vsCodeProtocol);
            vsCodeProtocol->SetDebugger(&debugger);
            Logger::log("SetDebugger for InterpreterVSCode");
            if (engineLogging)
                vsCodeProtocol->EngineLogging(logFilePath);
            if (!execFile.empty())
                vsCodeProtocol->OverrideLaunchCommand(execFile, execArgs);
            break;
        }
    }

    debugger.SetProtocol(protocol.get());

    IORedirectServer server(
        serverPort,
        [&protocol](std::string text) { protocol->EmitOutputEvent(OutputEvent(OutputStdOut, text)); },
        [&protocol](std::string text) { protocol->EmitOutputEvent(OutputEvent(OutputStdErr, text)); }
    );

    Logger::log("pidDebugee = " + std::to_string(pidDebuggee));
    if (pidDebuggee != 0)
    {
        debugger.Initialize();
        debugger.Attach(pidDebuggee);
        HRESULT Status = debugger.ConfigurationDone();
        if (FAILED(Status))
        {
            fprintf(stderr, "Error: 0x%x Failed to attach to %i\n", Status, pidDebuggee);
            return EXIT_FAILURE;
        }
    }

    protocol->CommandLoop();

    return EXIT_SUCCESS;
}
