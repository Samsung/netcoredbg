*get-vscodecmd.py* its a simple experimental script are needed for extracting the vscode client commands from the debugger output.

Debugger side are sensitive from some values of temporary objects in the vscode protocol. But, for some developers scenarios it can by useful. Also, debugger behavior can be unstable for this manner. But, for my workflow the script allowed me reproduce issue in debugger. Lets, look the usage example:
```
$ get-vscodecmd.py vscode_output > cmd
$ dos2unix cmd

$ netcoredbg --interpreter=vscode --engineLogging=/tmp < cmd
```
The first command produced the client commands with vscode-header like this "Content-Length: 347". Where `347` the payload size. The second command produced converted `\n\nn` to `\n\r\n\r`. And third command starts debugger in the batch mode.