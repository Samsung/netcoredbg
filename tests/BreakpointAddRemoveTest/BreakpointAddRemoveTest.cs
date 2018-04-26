/*
using System.IO;
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);

string filename = Path.GetFileName(TestSource);

Send(String.Format("3-break-insert -f {0}:{1}", filename, Lines["BREAK1"]));
r = Expect("3^done");
int id1 = r.Find("bkpt").FindInt("number");

Send(String.Format("4-break-insert -f {0}:{1}", filename, Lines["BREAK2"]));
r = Expect("4^done");
int id2 = r.Find("bkpt").FindInt("number");

Send("5-exec-run");
*/
using System;

namespace BreakpointAddRemoveTest
{
    class Program
    {
        static void Main(string[] args)
        {                                      // //@START@
/*
var r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["START"], r.Find("frame").FindInt("line"));

Send("6-exec-continue");
*/
            Console.WriteLine("Hello World!"); // //@BREAK1@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK1"], r.Find("frame").FindInt("line"));
Assert.Equal(id1, r.FindInt("bkptno"));

Send(String.Format("7-break-delete {0}", id2));
Expect("7^done");

Send("8-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
            Console.WriteLine("Hello World!"); // //@BREAK2@
        }
    }
}
