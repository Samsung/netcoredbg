/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
Send("3-exec-run");

var r = Expect("*stopped");
Assert.Equal(r.FindString("reason"), "entry-point-hit");
Assert.Equal(r.Find("frame").FindInt("line"), Lines["START"]);

Send("4-exec-step");
r = Expect("*stopped");
Assert.Equal(r.FindString("reason"), "end-stepping-range");
Assert.Equal(r.Find("frame").FindInt("line"), Lines["STEP1"]);

Send("5-exec-step");
r = Expect("*stopped");
Assert.Equal(r.FindString("reason"), "end-stepping-range");
Assert.Equal(r.Find("frame").FindInt("line"), Lines["STEP2"]);

Send("6-gdb-exit");
*/

using System;

namespace simple_stepping
{
    class Program
    {
        static void Main(string[] args)
        {                                      // //@START@
            Console.WriteLine("Hello World!"); // //@STEP1@
        }                                      // //@STEP2@
    }
}
