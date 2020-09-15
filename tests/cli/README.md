This directory contains test program used to test "cli" interface of netcoredbg.

```
Usage: corerun test.dll <function> [args...]
Function should be one of the following:
assert     -- call Debug.Assert
fail       -- call Debug.Fail
null       -- cause NullReferenceException
range      -- cause IndexOutOfRange exception
key        -- cause KeyNotFound exception
overflow   -- cause OverflowException
sigsegv    -- cause SIGSEGV signal
exception  -- throw and catch test exception
task       -- run test task
delegate   -- call function via delegate
wait       -- wait for input from stdin
vars       -- test variables in debugger (break test.cs:100)
function   -- test breakpoints in debugger (break test.cs:30
output     -- test stdout/stderr/debug output
```

