/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
Send("3-exec-run");

var r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["START"], r.Find("frame").FindInt("line"));
*/

using System;

namespace ExceptionTest
{
    class P {
        public int x {
            get { return 111; }
        }
    }

    class Program
    {
        static void MyFunction(string s)
        {
            throw new Exception("test exception"); // //@THROW1@
        }
        static void MyFunction(int a)
        {
        }
        static void Main(string[] args)
        {                  // //@START@
            P p = new P(); // //@STEP1@
/*
var try1Lines = new string[] { "STEP1", "TRY1", "TRY2", "TRY3", "CATCH1", "CATCH2", "CATCH3", "CATCH4" };
foreach (string tag in try1Lines)
{
    Send("-exec-next");
    r = Expect("*stopped");
    Assert.Equal("end-stepping-range", r.FindString("reason"));
    Assert.Equal(Lines[tag], r.Find("frame").FindInt("line"));
}
*/
            try
            {                       // //@TRY1@
                try
                {                   // //@TRY2@
                    MyFunction(""); // //@TRY3@
                }
                catch(Exception e)  // //@CATCH1@
                {                   // //@CATCH2@
                    int a = 1;      // //@CATCH3@
                }                   // //@CATCH4@
/*
// Check stack frames location

Send("-stack-list-frames");
r = Expect("^done");

NamedResultValue[] frames = r.Find<ResultListValue>("stack").Content;
Assert.True(frames.Length >= 3);

Assert.Equal("frame", frames[0].Name);
Assert.Equal("frame", frames[1].Name);
Assert.Equal("frame", frames[2].Name);

Assert.Equal(0, frames[0].Value.FindInt("level"));
Assert.Equal(1, frames[1].Value.FindInt("level"));
Assert.Equal(2, frames[2].Value.FindInt("level"));

Assert.Equal(TestSource, frames[0].Value.FindString("fullname"));
Assert.Equal(TestSource, frames[1].Value.FindString("fullname"));
Assert.Equal(TestSource, frames[2].Value.FindString("fullname"));

Assert.Equal(Lines["CATCH4"], frames[0].Value.FindInt("line"));
Assert.Equal(Lines["THROW1"], frames[1].Value.FindInt("line"));
Assert.Equal(Lines["TRY3"],   frames[2].Value.FindInt("line"));

// Check local variables

Send("-stack-list-variables");
r = Expect("^done");
ResultValue[] variables = r.Find<ValueListValue>("variables").AsArray<ResultValue>();
Func<string, string> getVarValue = (string name) => Array.Find(variables, v => v.FindString("name") == name).FindString("value");

Assert.Equal("{System.Exception}", getVarValue("$exception"));
Assert.Equal("{System.Exception}", getVarValue("e"));
Assert.Equal("1",                  getVarValue("a"));
Assert.Equal("{ExceptionTest.P}",  getVarValue("p"));
Assert.Equal("{string[0]}",        getVarValue("args"));

// Execute property and check its value

Send(String.Format("-var-create - * \"{0}\"", "p.x"));
r = Expect("^done");
Assert.Equal("111", r.FindString("value"));
Assert.Equal("0", r.FindString("numchild"));
Assert.Equal("int", r.FindString("type"));
Assert.Equal("p.x", r.FindString("exp"));
*/
                int b = 2;          // //@TRY4@
                try
                {
                    MyFunction("");
                }
                catch(Exception e)
                {
                    int c = 1;
                    throw new Exception("my exception");
                }
            }
            catch(Exception e)
            {
                MyFunction(1);
            }
            int d = 1; // //@BREAK@
        }
    }
}
/*
Send("-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
