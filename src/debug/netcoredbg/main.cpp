// Copyright (c) 2017 Samsung Electronics Co., LTD
// Distributed under the MIT License.
// See the LICENSE file in the project root for more information.

#include "common.h"

#include "manageddebugger.h"
#include "miprotocol.h"


static void print_help()
{
    fprintf(stderr,
        ".NET Core debugger for Linux/macOS.\n"
        "\n"
        "Options:\n"
        "--attach <process-id>                 Attach the debugger to the specified process id.\n"
        "--interpreter=mi                      Puts the debugger into MI mode.\n");
}

int main(int argc, char *argv[])
{
    DWORD pidDebuggee = 0;

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
    MIProtocol protocol;

    protocol.SetDebugger(&debugger);
    debugger.SetProtocol(&protocol);

    if (pidDebuggee != 0)
    {
        HRESULT Status = debugger.AttachToProcess(pidDebuggee);
        if (FAILED(Status))
        {
            fprintf(stderr, "Error: 0x%x Failed to attach to %i\n", Status, pidDebuggee);
            return EXIT_FAILURE;
        }
    }

    protocol.CommandLoop();

    return EXIT_SUCCESS;
}
