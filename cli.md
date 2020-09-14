# Using netcoredbg with CLI (Command Line Interface)
## Command reference
```
        Command         Shortcut              Description
backtrace                  bt         Print backtrace info.
break <file.ext:XXX>       b          Set breakpoint at source file.ext:line_number.
break <func_name>          b          Set breakpoint at function func_name().
continue                   c          Continue debugging after stop/pause.
delete <X>                 d          Delete breakpoint number X.
file <filename>                       Load executable to debug.
finish                                Continue execution until the current stack frame returns.
help                       h          Print this help message.
next                       n          Step program.
print <name>               p          Print variable's value.
quit                       q          Quit the debugging session.
run                        r          Start debugging program.
step                       s          Step program until it reaches the next source line.
set-args                              Set the debugging program arguments.
```
### Start debugger
To start the debugger in CLI mode just type 
```
$ netcoredbg --interpreter=cli
```
### Loading debugging program
To load the debugging program use "file" command and provide the name of an executable you use to start your assembly, for example dotnet:
```
cli> file dotnet
```
The name of the assembly you want to debug and all the required parameters can be set with "set-args" command, for example:
```
cli> set-args hello.dll param1 param2
```

### Running debugging program
Now the debugger is ready to run your assembly. The debugging information is not loaded at this moment and will be available after your program starts. Anyway you can set any breakpoint here using "break" command (see below). Please keep in mind that all the breakpoints set here will have "pending" status until "hit entry point" event will occur. Example:
```
cli> run
library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/3.1.6/System.Private.CoreLib.dll
no symbols loaded, base address: 0x7f5216d00000, size: 9348096(0x8ea400)

thread created, id: 17015

library loaded: /home/user/work/hello/bin/Debug/netcoreapp3.1/hello.dll
symbols loaded, base address: 0x7f5292621000, size: 5632(0x1600)

library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/3.1.6/System.Runtime.dll
no symbols loaded, base address: 0x7f5217720000, size: 241152(0x3ae00)

library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/3.1.6/System.Console.dll
no symbols loaded, base address: 0x7f5217780000, size: 376320(0x5be00)

stopped, reason: entry point hit, thread id: 17015, stopped threads: all, frame={
    /home/user/work/hello/Program.cs:27  (col: 9 to line: 27 col: 10)
    clr-addr: {module-id {a8fcf093-80eb-40db-bd6c-376df182eda3}, method-token: 0x06000001 il-offset: 0, native offset: 55}
    hello.Program.Main(), addr: 0x00007ffcecd04b70
}

```
### Setting breakpoints
After the progam was started and its entry point has been hit it's a good time to set your breakpoints. Now you can set 2 kinds of breakpoints - by function name and by filename:line-number. The conditional and exceptional breakpoints are not supported yet. Examples:

Set breakpoint at a line 66 of Program.cs:
```
cli> break Program.cs:66
^done, Breakpoint 1 at /home/oleg/work/hello/Program.cs:66
```
Set breakpoint at function func2():
```
cli> b func2
^done, Breakpoint 2 at func2()
```
### Deleting breakpoints
To delete breakpoint just type "delete" and it's number:
```
cli> delete 2
^done
```
### Continue execution
To continue your program's execution just type:
```
cli> continue
```
or
```
cli> c
```
### Stepping
There are 3 commands available for stepping your program: "step", "next" and "finish".

"step" command executes you program until it reaches the next source line.

"next" command executes your program until it reaches the source line below the current line.

"finish" command continues execution until the current stack frame returns.

### Print variables
To print a value of a variable just type "print" or "p" and a variable's name, for example:
```
cli> print i
^done,
i = 3
cli> p s.a
^done,
s.a = 2
```
### Command history
The CLI remembers the history of the commands you've issued during the current and previous sessions. You can move back and forth in the history pressing `<up>` and `<down>` arrows. You can also edit the command line and search your history in forward `<ctrl>-s` or reverse `<ctrl>-r` directions.

### Quit
To leave your debugging session just type:
```
cli> quit
```
All your debugging data for the current session will be lost.
