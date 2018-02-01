// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include "manageddebugger.h"
#include "miprotocol.h"
#include "vscodeprotocol.h"


static void print_help()
{
    fprintf(stdout,
        ".NET Core debugger for Linux/macOS.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n"
        "--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.\n"
        "--engineLogging[=<path to log file>]  Enable logging to VsDbg-UI or file for the engine.\n"
        "                                      Only supported by the VsCode interpreter.\n"
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
    std::string logFilePath;

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
        else
        {
            fprintf(stderr, "Error: Unknown option %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }

    ManagedDebugger debugger;
    std::unique_ptr<Protocol> protocol;

    switch(interpreterType)
    {
        case InterpreterMI:
            if (engineLogging)
            {
                fprintf(stderr, "Error: Engine logging is only supported in VsCode interpreter mode.\n");
                return EXIT_FAILURE;
            }
            protocol.reset(new MIProtocol());
            static_cast<MIProtocol*>(protocol.get())->SetDebugger(&debugger);
            break;
        case InterpreterVSCode:
        {
            VSCodeProtocol *vsCodeProtocol = new VSCodeProtocol();
            protocol.reset(vsCodeProtocol);
            vsCodeProtocol->SetDebugger(&debugger);
            if (engineLogging)
                vsCodeProtocol->EngineLogging(logFilePath);
            break;
        }
    }

    debugger.SetProtocol(protocol.get());

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
