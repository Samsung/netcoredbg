/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
Send("3-exec-run");

var r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["START"], r.Find("frame").FindInt("line"));

Send(String.Format("4-break-insert -f {0}:{1}", TestSource, Lines["BREAK"]));
Expect("4^done");

Send("5-exec-continue");
*/

using System;

namespace values
{
    class Program
    {
        static void Main(string[] args)
        {              // //@START@
            decimal d = 12345678901234567890123456m;
            decimal long_zero_dec = 0.00000000000000000017M;
            decimal short_zero_dec = 0.17M;
            int x = 1; // //@BREAK@
            /*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK"], r.Find("frame").FindInt("line"));

Send(String.Format("6-var-create - * \"{0}\"", "d"));
r = Expect("6^done");
Assert.Equal("12345678901234567890123456", r.FindString("value"));
Assert.Equal("d", r.FindString("exp"));
Assert.Equal("0", r.FindString("numchild"));
Assert.Equal("decimal", r.FindString("type"));

Send(String.Format("7-var-create 8 * \"{0}\"", "long_zero_dec"));
r = Expect("7^done");
Assert.Equal("0.00000000000000000017", r.FindString("value"));

Send(String.Format("8-var-create 8 * \"{0}\"", "short_zero_dec"));
r = Expect("8^done");
Assert.Equal("0.17", r.FindString("value"));
             */
        }
    }
}
/*
Send("9-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
