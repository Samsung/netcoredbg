// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include "manageddebugger.h"
#include "miprotocol.h"
#include "vscodeprotocol.h"


static void print_help()
{
    fprintf(stderr,
        ".NET Core debugger for Linux/macOS.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n"
        "--interpreter=vscode                  Puts the debugger into VS Code Debugger mode.\n");
}

int main(int argc, char *argv[])
{
    DWORD pidDebuggee = 0;

    enum InterpreterType
    {
        InterpreterMI,
        InterpreterVSCode
    } interpreterType = InterpreterMI;

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
            protocol.reset(new MIProtocol());
            static_cast<MIProtocol*>(protocol.get())->SetDebugger(&debugger);
            break;
        case InterpreterVSCode:
            protocol.reset(new VSCodeProtocol());
            static_cast<VSCodeProtocol*>(protocol.get())->SetDebugger(&debugger);
            break;
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
