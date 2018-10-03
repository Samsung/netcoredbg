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

Send(String.Format("5-break-insert -f {0}:{1}", filename, Lines["BREAK3"]));
r = Expect("5^done");
int id3 = r.Find("bkpt").FindInt("number");

Send("6-exec-run");
*/

using System;

namespace ExpressionsTest
{
    class Program
    {
        static void Main(string[] args)
        {                                               // //@START@
/*
var r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["START"], r.Find("frame").FindInt("line"));

Send("7-exec-continue");
*/
            int a = 10;
            int b = 11;
            TestStruct tc = new TestStruct(a + 1, b);
            string str1 = "string1";
            string str2 = "string2";
            int c = tc.b + b;                           // //@BREAK1@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK1"], r.Find("frame").FindInt("line"));
Assert.Equal(id1, r.FindInt("bkptno"));

Send(String.Format("8-var-create - * \"{0}\"", "a + b"));
r = Expect("8^done");
Assert.Equal("21", r.FindString("value"));

Send(String.Format("9-var-create - * \"{0}\"", "tc.a + b"));
r = Expect("9^done");
Assert.Equal("22", r.FindString("value"));

Send(String.Format("10-var-create - * \"{0}\"", "str1 + str2"));
r = Expect("10^done");
Assert.Equal("\"string1string2\"", r.FindString("value"));

Send("11-exec-continue");
*/
            {
                int d = 99;
                int e = c + a;                          // //@BREAK2@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK2"], r.Find("frame").FindInt("line"));
Assert.Equal(id2, r.FindInt("bkptno"));

Send(String.Format("12-var-create - * \"{0}\"", "d + a"));
r = Expect("12^done");
Assert.Equal("109", r.FindString("value"));

Send("13-exec-continue");
*/
            }

            Console.WriteLine(str1 + str2);

            tc.IncA();

            Console.WriteLine("Hello World!");
        }
    }

    struct TestStruct
    {
        public int a;
        public int b;

        public TestStruct(int x, int y)
        {
            a = x;
            b = y;
        }

        public void IncA()
        {
            a++;                                        // //@BREAK3@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK3"], r.Find("frame").FindInt("line"));
Assert.Equal(id3, r.FindInt("bkptno"));

Send(String.Format("14-var-create - * \"{0}\"", "a + 1"));
r = Expect("14^done");
Assert.Equal("12", r.FindString("value"));

Send("15-exec-continue");
*/
        }
    }
/*
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
}
