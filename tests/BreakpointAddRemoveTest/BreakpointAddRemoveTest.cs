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

Send(String.Format("6-break-insert -f -c \"x>50\" BreakpointAddRemoveTest.Program.TestFunc2"));
r = Expect("6^done");
int id4 = r.Find("bkpt").FindInt("number");

Send(String.Format("7-break-insert -f Program.TestFunc3"));
r = Expect("7^done");
int id5 = r.Find("bkpt").FindInt("number");

Send(String.Format("8-break-insert -f TestFunc4(int)"));
r = Expect("8^done");
int id6 = r.Find("bkpt").FindInt("number");

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

Send("9-exec-continue");
*/
            Console.WriteLine("Hello World!"); // //@BREAK1@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK1"], r.Find("frame").FindInt("line"));
Assert.Equal(id1, r.FindInt("bkptno"));

Send(String.Format("10-break-delete {0}", id2));
Expect("10^done");
Send("11-exec-continue");
*/
                TestFunc(10);
                TestFunc(21);
                TestFunc(9);
                TestFunc2(11);
                TestFunc2(90);

                TestFunc3(true);
                TestFunc3();

                TestFunc4();
                TestFunc4(50);

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
Send("12-exec-continue");
*/
        }

        static void TestFunc2(int x)
        {                                       // //@FUNC_BREAK@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["FUNC_BREAK"], r.Find("frame").FindInt("line"));
Assert.Equal(id4, r.FindInt("bkptno"));
Send("13-exec-continue");
*/
                x++;
        }

        static int TestFunc3(bool iii)
        {                                       // //@FUNC_BREAK2@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["FUNC_BREAK2"], r.Find("frame").FindInt("line"));
Assert.Equal(id5, r.FindInt("bkptno"));
Send("14-exec-continue");
*/
                return 10;
        }

        static int TestFunc3()
        {                                       // //@FUNC_BREAK3@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["FUNC_BREAK3"], r.Find("frame").FindInt("line"));
Assert.Equal(id5, r.FindInt("bkptno"));
Send("15-exec-continue");
*/
                return 100;
        }

        static int TestFunc4(int a)
        {                                       // //@FUNC_BREAK4@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["FUNC_BREAK4"], r.Find("frame").FindInt("line"));
Assert.Equal(id6, r.FindInt("bkptno"));
Send("16-exec-continue");
*/
                return 50;
        }

        static int TestFunc4()
        {
                return 10;
        }
    }
/*
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
}
