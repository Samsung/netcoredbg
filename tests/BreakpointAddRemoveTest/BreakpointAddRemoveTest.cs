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

Send(String.Format("5-break-insert -f -c \"x>20\" {0}:{1}", filename, Lines["BREAK3"]));
r = Expect("5^done");
int id3 = r.Find("bkpt").FindInt("number");

Send("-exec-run");
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
*/
                TestFunc(10);
                TestFunc(21);
                TestFunc(9);

                Console.WriteLine("Hello World!"); // //@BREAK2@
        }

        static void TestFunc(int x)
        {
                x++;                            // //@BREAK3@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK3"], r.Find("frame").FindInt("line"));
Assert.Equal(id3, r.FindInt("bkptno"));
Send("9-exec-continue");
*/
        }
    }
/*
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
}
