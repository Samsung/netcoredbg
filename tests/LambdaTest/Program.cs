/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);

Send(String.Format("3-break-insert -f {0}:{1}", "Program.cs", Lines["LAMBDAENTRY"]));
var r = Expect("3^done");

Send("4-exec-run");
r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));

Send("5-exec-continue");
r = Expect("5^running");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["LAMBDAENTRY"], r.Find("frame").FindInt("line"));

Send(String.Format("6-var-create - * \"{0}\"", "staticVar"));
r = Expect("6^done");
Assert.Equal("\"staticVar\"", r.FindString("value"));

Send(String.Format("7-var-create - * \"{0}\"", "mainVar"));
r = Expect("7^done");
Assert.Equal("\"mainVar\"", r.FindString("value"));

Send(String.Format("8-var-create - * \"{0}\"", "argVar"));
r = Expect("8^done");
Assert.Equal("\"argVar\"", r.FindString("value"));

Send(String.Format("9-var-create - * \"{0}\"", "localVar"));
r = Expect("9^done");
Assert.Equal("\"localVar\"", r.FindString("value"));
*/

using System;
namespace LambdaTest
{
    delegate void Lambda(string argVar);

    class Program
    {
        static string staticVar = "staticVar";

        static void Main(string[] args)
        {   // //@START@
            string mainVar = "mainVar";

            Lambda lambda = (argVar) => {
                string localVar = "localVar";

                Console.WriteLine(staticVar); // //@LAMBDAENTRY@
                Console.WriteLine(mainVar);
                Console.WriteLine(argVar);
                Console.WriteLine(localVar);
            };

            lambda("argVar");
        }
    }
}

