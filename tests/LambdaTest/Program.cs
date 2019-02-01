/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);

Send(String.Format("3-break-insert -f {0}:{1}", TestSource, Lines["LAMBDAENTRY1"]));
r = Expect("3^done");

Send(String.Format("4-break-insert -f {0}:{1}", TestSource, Lines["LAMBDAENTRY2"]));
var r = Expect("4^done");

Send("5-exec-run");
r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));

Send("6-exec-continue");
r = Expect("6^running");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["LAMBDAENTRY1"], r.Find("frame").FindInt("line"));

Send("7-stack-list-variables");
r = Expect("7^done");
ResultValue[] variables = r.Find<ValueListValue>("variables").AsArray<ResultValue>();
Func<string, string> getVarValue = (string name) => Array.Find(variables, v => v.FindString("name") == name).FindString("value");

Assert.Equal(variables.Length, 6);
Assert.Equal("{LambdaTest.Class1.Class2}", getVarValue("this"));
Assert.Equal("{LambdaTest.Lambda}", getVarValue("lambda2"));
Assert.Equal("\"funcVar\"",     getVarValue("funcVar"));
Assert.Equal("\"funcArg\"",     getVarValue("funcArg"));
Assert.Equal("\"localVar1\"",   getVarValue("localVar1"));
Assert.Equal("\"argVar1\"",     getVarValue("argVar1"));

string [] accessibleVars1 = {
    "staticVar1",
    "staticVar2",
    "instanceVar",
    "funcVar",
    "funcArg",
    "localVar1",
    "argVar1"
};

foreach (string v in accessibleVars1)
{
    Send(String.Format("-var-create - * \"{0}\"", v));
    r = Expect("^done");
    Assert.Equal($"\"{v}\"", r.FindString("value"));
}

Send("8-exec-continue");
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["LAMBDAENTRY2"], r.Find("frame").FindInt("line"));

Send("9-stack-list-variables");
r = Expect("9^done");
variables = r.Find<ValueListValue>("variables").AsArray<ResultValue>();

Assert.Equal(variables.Length, 7);
Assert.Equal("{LambdaTest.Class1.Class2}", getVarValue("this"));
Assert.Equal("\"funcVar\"",     getVarValue("funcVar"));
Assert.Equal("\"funcArg\"",     getVarValue("funcArg"));
Assert.Equal("\"localVar1\"",   getVarValue("localVar1"));
Assert.Equal("\"argVar1\"",     getVarValue("argVar1"));
Assert.Equal("\"localVar2\"",   getVarValue("localVar2"));
Assert.Equal("\"argVar2\"",     getVarValue("argVar2"));

string [] accessibleVars2 = {
    "staticVar1",
    "staticVar2",
    "instanceVar",
    "funcVar",
    "funcArg",
    "localVar1",
    "argVar1",
    "localVar2",
    "argVar2"
};

foreach (string v in accessibleVars2)
{
    Send(String.Format("-var-create - * \"{0}\"", v));
    r = Expect("^done");
    Assert.Equal($"\"{v}\"", r.FindString("value"));
}

*/

using System;
namespace LambdaTest
{
    delegate void Lambda(string argVar);

    class Class1
    {
        static string staticVar1 = "staticVar1";
        class Class2
        {

            static string staticVar2 = "staticVar2";

            string instanceVar = "instanceVar";

            public void Func(string funcArg)
            {
                string funcVar = "funcVar";

                Lambda lambda1 = (argVar1) => {
                    string localVar1 = "localVar1";

                    Lambda lambda2 = (argVar2) => {
                            string localVar2 = "localVar2";

                            Console.WriteLine(staticVar1); // //@LAMBDAENTRY2@
                            Console.WriteLine(staticVar2);
                            Console.WriteLine(instanceVar);
                            Console.WriteLine(funcVar);
                            Console.WriteLine(funcArg);
                            Console.WriteLine(localVar1);
                            Console.WriteLine(argVar1);
                            Console.WriteLine(localVar2);
                            Console.WriteLine(argVar2);
                    };
                    Console.WriteLine(staticVar1); // //@LAMBDAENTRY1@
                    Console.WriteLine(staticVar2);
                    Console.WriteLine(instanceVar);
                    Console.WriteLine(funcVar);
                    Console.WriteLine(funcArg);
                    Console.WriteLine(localVar1);
                    Console.WriteLine(argVar1);
                    lambda2("argVar2");
                };
                lambda1("argVar1");
            }
        }

        static void Main(string[] args)
        {   // //@START@
            var c = new Class2();
            c.Func("funcArg");
        }
    }
}

/*
Send("-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/