# Using NetCoreDbg with CLI (Command Line Interface)

## Fast start

To start debugger and run program with one command just type
```
$ netcoredbg --interpreter=cli -- dotnet hello.dll param1 param2
```

Note: to debug Release build of dlls with pdbs Just-My-Code should be disabled, see below.

## Command reference
```
command    alias  args               
--------------------------------------
backtrace  bt     [all][--thread TID]  Print backtrace info.
break      b      <loc>                Set breakpoint at specified location, where the
                                       location might be filename.cs:line or function name.
                                       Optional, module name also could be provided as part
                                       of location: module.dll!filename.cs:line
                                       GDB-like command syntax:
                                       break file_name:line_num
                                       break func_name
                                       break func_name(args)
                                       break assembly.dll!func_name
                                       break ... if condition
catch                                  Set exception breakpoints.
continue   c                           Continue debugging after stop/pause.
delete     clear  <num>                Delete breakpoint with specified number.
detach                                 Detach from the debugged process.
disable                                Disable breakpoint N.
enable                                 Enable breakpoint N.
file              <file>               load executable file to debug.
finish                                 Continue execution till end of the current function.
frame      f                           Select & display stack frame.
interrupt                              Interrupt program execution, stop all threads.
list       l                           List source code lines, 5 by default.
next       n                           Step program, through function calls.
print      p      <expr>               Print variable value or evaluate an expression.
quit       q                           Quit the debugger.
run        r                           Start debugged program.
attach                                 Attach to the debugged process.
detach                                 Detach from the debugged process.
step       s                           Step program until a different source line.
source            <file>               Read commands from a file.
wait                                   Wait until debugee stops (in async. mode)

command    alias  args               
---------------------------------------
set               args...              Set miscellaneous options (see 'help set')
info              <topic>              Show misc. things about the program being debugged.
save              args...              Save misc. things to the files.
help              [topic]              Show help on specified topic or print
                                       this help message (if no argument specified).
```

### Preparing debug information
For showing classes and function names by NCDB required to have PDB files. But for showing source code of debuggee application also need to add tag EmbedAllSources to csproj file:

```<EmbedAllSources>true</EmbedAllSources>```

### Start debugger
To start the debugger in CLI mode just type 
```
$ netcoredbg --interpreter=cli
```

To load the debugging program file command with the name of an executable used to start assembly as argument should be used, for example dotnet:
```
ncdb> file dotnet
```

The name of the assembly for debugging and all the required parameters could be set with "set args" command, for example:
```
ncdb> set args hello.dll param1 param2
```

### Debugging Release build of dlls

To debug Release build of dlls with pdbs Just-My-Code should be disabled:
```
ncdb> set just-my-code 0
```

### Running debugging program
Now the debugger is ready to run your assembly. The debugging information is not loaded at this moment and will be available after debuggee program starts. But breakpoints could be set here using `break` command (see below). Keep in mind that all breakpoints set here will have "pending" status until "hit entry point" event will occur. Debugging information is provided by *.pdb file. If the debugger can't find the correspoinding *.pdb the debugging will not be possible, even hitting the entry point will not pause the debuggee process. Example:
```
ncdb> run
thread created, id: 15498

library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/5.0.11/System.Private.CoreLib.dll
no symbols loaded, base address: 0x7f69fe5b0000, size: 9470976(0x908400)

library loaded: /home/user/src/sample/bin/Debug/net5.0/sample.dll
symbols loaded, base address: 0x7f6a79e01000, size: 4608(0x1200)
breakpoint modified,  Breakpoint 2 at foo()
breakpoint modified,  Breakpoint 1 at Main()

library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/5.0.11/System.Runtime.dll
no symbols loaded, base address: 0x7f69ff000000, size: 230912(0x38600)

library loaded: /usr/share/dotnet/shared/Microsoft.NETCore.App/5.0.11/System.Console.dll
no symbols loaded, base address: 0x7f69ff070000, size: 376832(0x5c000)

stopped, reason: breakpoint 1 hit, thread id: 15498, stopped threads: all, times= 0, frame={sample.Program.Main() at /home/user/src/sample/Program.cs:13
```

### Setting breakpoints
There are set 2 kinds of breakpoints - by function name and by filename:line-number.
Examples:

Set breakpoint at a line `66` of `Program.cs`:
```
ncdb> break Program.cs:66
^done, Breakpoint 1 at /home/oleg/work/hello/Program.cs:66
```
Set breakpoint at function `func2()`:
```
ncdb> b func2
^done, Breakpoint 2 at func2()
```
Set breakpoint at `func1(int i)` member function of `Program` class of `hello` namespace:
```
ncdb> b hello.Program.func1(int)
^done, Breakpoint 3 at func1()
```
### Deleting breakpoints
To delete breakpoint just type `delete` and it number:
```
ncdb> delete 2
^done
```

### Showing all breakpoints
`info` command with `break` argument shows all breakpoints. Information about hits and if this break point was resolved also will be shown:
```
ncdb> info break
    #  Enb  Rslvd  Hits       Source/Function
---------------------------------------------
    1    y  y      1          Main
    2    y  y      1          foo
```

### Continue execution
To continue program's execution type:
```
ncdb> continue
```
or
```
ncdb> c
```

### Stepping
There are 3 commands available for stepping your program: `step`, `next` and `finish`.

`step` command executes program until it reaches the next source line.
`next` command executes program until it reaches the source line below the current line.
`finish` command continues execution until the current stack frame returns.

### Print variables
To print a value of a variable type `print` or `p` and a variable's name, for example:
```
ncdb> print i
^done,
i = 3
cli> p s.a
^done,
s.a = 2
```

### Command history
The CLI remembers the history of the commands which user used during the current and previous sessions. It's possible to move back and forth in the history pressing <up> and <down> arrows. Also possible to edit the command line and search history in forward <ctrl>-s or reverse <ctrl>-r directions.

### Showing source code
NCDB allows to show source code of debuggee program, while execution stopped at breakpoint:
```
ncdb> list
8	    		Console.WriteLine("foo!!!");
9	    	}
10	
11	    	static int A = 10;
12	        static void Main(string[] args)
13	        {
14	            Console.WriteLine("Hello World!");
15	            foo();
16	        }
17	    }
```
`list` command also support various arguments:

`list ,XX` - print 10 lines ending with `XX`

`list XX,` - print 10 lines starting with `XX` line

`list XX,YY` - print lines from `XX` to `YY`

`list -` - print 10 lines just before the last printed

`list +` - print 10 lines just after the last printed

`list XX` - print 10 lines with `XX` centered


### Showing back trace
To see stack of calls at the current break point type:
```
ncdb> bt
#0 sample.Program.foo() at /home/alexander/src/sample/Program.cs:7
#1 sample.Program.Main() at /home/alexander/src/sample/Program.cs:15
```

### Quit
To leave debugging session type:
```
ncdb> quit
```

