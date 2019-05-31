/*
using System.IO;
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);

string filename = Path.GetFileName(TestSource);

Send("3-break-exception-insert throw+user-unhandled A B C");
var r = Expect("3^done");
Send("4-break-exception-delete 3 2 1");
r = Expect("4^done");

//////// Silent removing of previous global exception filter
Send("200-break-exception-insert throw *");
r = Expect("200^done");
int be200 = r.Find("bkpt").FindInt("number");

Send("201-break-exception-insert throw+user-unhandled *");
r = Expect("201^done");
int be201 = r.Find("bkpt").FindInt("number");

Send(String.Format("202-break-exception-delete {0}", be200));
r = Expect("202^error");

Send(String.Format("203-break-exception-delete {0}", be201));
r = Expect("203^done");
////////

Send(String.Format("5-break-insert -f {0}:{1}", filename, Lines["PIT_STOP_A"]));
r = Expect("5^done");
int b1 = r.Find("bkpt").FindInt("number");

Send(String.Format("6-break-insert -f {0}:{1}", filename, Lines["PIT_STOP_B"]));
r = Expect("6^done");
int b2 = r.Find("bkpt").FindInt("number");

Send("7-exec-run");

r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["MAIN"], r.Find("frame").FindInt("line"));

//////// Expected result => Not found any exception breakpoits.
//////// "State: !Thow() && !UserUnhandled() name := '*'";
Send("8-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

//////// Expected result => Not found any exception breakpoits.
//////// "State: !Thow() && UserUnhandled() name := '*'";
Send("10-break-exception-insert user-unhandled *");
r = Expect("10^done");
int be1 = r.Find("bkpt").FindInt("number");

Send("11-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("12-break-exception-delete {0}", be1));
r = Expect("12^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
//////// "State: Thow() && !UserUnhandled() name := '*'";
Send("13-break-exception-insert throw *");
r = Expect("13^done");
int be2 = r.Find("bkpt").FindInt("number");

Send("14-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("15-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("17-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("18-break-exception-delete {0}", be2));
r = Expect("18^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
//////// "State: Thow() && UserUnhandled() name := '*'";
Send("19-break-exception-insert throw+user-unhandled *");
r = Expect("19^done");
int be3 = r.Find("bkpt").FindInt("number");

Send("20-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("21-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("22-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("23-break-exception-delete {0}", be3));
r = Expect("23^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
//////// "State: Thow() && UserUnhandled() name := '*'";
Send("19-break-exception-insert throw+user-unhandled *");
r = Expect("19^done");
int be4 = r.Find("bkpt").FindInt("number");

Send("20-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("21-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("22-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("23-break-exception-delete {0}", be4));
r = Expect("23^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
//////// "State: Thow() && UserUnhandled()";
//////// "name := System.DivideByZeroException";
Send("24-break-exception-insert throw+user-unhandled System.DivideByZeroException");
r = Expect("24^done");
int be5 = r.Find("bkpt").FindInt("number");

Send("25-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("26-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("27-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("28-break-exception-delete {0}", be5));
r = Expect("28^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
////////  "State: Thow()";
////////  "name := System.DivideByZeroException";
////////  "State: UserUnhandled()";
////////  "name := System.DivideByZeroException";
Send("29-break-exception-insert throw System.DivideByZeroException");
r = Expect("29^done");
int be6 = r.Find("bkpt").FindInt("number");

Send("30-break-exception-insert user-unhandled System.DivideByZeroException");
r = Expect("30^done");
int be7 = r.Find("bkpt").FindInt("number");

Send("31-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("32-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("33-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("34-break-exception-delete {0}", be7));
r = Expect("34^done");

Send(String.Format("35-break-exception-delete {0}", be6));
r = Expect("35^done");

//////// Expected result => Raised EXCEPTION_A and EXCEPTION_B.
//////// "State: Thow() && UserUnhandled()";
//////// "name := System.DivideByZeroExceptionWrong and *";
Send("36-break-exception-insert throw+user-unhandled *");
r = Expect("36^done");
int be8 = r.Find("bkpt").FindInt("number");

Send("37-break-exception-insert throw+user-unhandled DivideByZeroExceptionWrong");
r = Expect("37^done");
int be9 = r.Find("bkpt").FindInt("number");

Send("38-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_A"], r.Find("frame").FindInt("line"));

Send("39-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_B"], r.Find("frame").FindInt("line"));

Send("40-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_A"], r.Find("frame").FindInt("line"));

Send(String.Format("41-break-exception-delete {0}", be8));
r = Expect("41^done");

Send(String.Format("42-break-exception-delete {0}", be9));
r = Expect("42^done");

//////// Expected result => Not found any exception breakpoits.
//////// "State: Thow() && UserUnhandled()";
//////// "name := System.DivideByZeroExceptionWrong";

Send("36-break-exception-insert throw+user-unhandled System.DivideByZeroExceptionWrong");
r = Expect("36^done");
int be10 = r.Find("bkpt").FindInt("number");

Send("37-exec-continue");

r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP_B"], r.Find("frame").FindInt("line"));

Send(String.Format("38-break-exception-delete {0}", be10));
r = Expect("38^done");

//////// Expected result => Raised EXCEPTION_C, EXCEPTION_D, EXCEPTION_E and exit after unhandled EXCEPTION_E.
//////// "State: !Thow() && UserUnhandled()";
//////// "name := AppException";

//////// Test of setting unhandled - read only mode.
Send("39-break-exception-insert unhandled AppException");
r = Expect("39^done");

Send("40-break-exception-insert user-unhandled AppException");
r = Expect("40^done");
int be11 = r.Find("bkpt").FindInt("number");

Send("41-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_C"], r.Find("frame").FindInt("line"));

Send("42-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_D"], r.Find("frame").FindInt("line"));

Send("43-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_E"], r.Find("frame").FindInt("line"));

Send("44-exec-continue");

r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Assert.Equal(Lines["EXCEPTION_E"], r.Find("frame").FindInt("line"));

Send("45-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));

Send(String.Format("46-break-exception-delete {0}", be11));
r = Expect("46^done");

Send(String.Format("99-break-delete {0}", b1));
Expect("99^done");
Send(String.Format("100-break-delete {0}", b2));
Expect("100^done");

Send("-gdb-exit");
*/

using System;
using System.Threading;

public class AppException : Exception
{
    public AppException(String message) : base(message) { }
    public AppException(String message, Exception inner) : base(message, inner) { }
}

public class Test
{
    public void TestSystemException()
    { // //@PIT_STOP_A@
        int zero = 1 - 1;
        try
        {
            int a = 1 / zero; // //@EXCEPTION_A@
        }
        catch (Exception e)
        {
            Console.WriteLine($"Implicit Exception: {e.Message}");
        }
        try
        {
            throw new System.DivideByZeroException(); // //@EXCEPTION_B@
        }
        catch (Exception e)
        {
            Console.WriteLine($"Explicit Exception: {e.Message}");
        }
        Console.WriteLine("Complete system exception test\n");
    }
    public void TestAppException()
    { // //@PIT_STOP_B@
        try
        {
            CatchInner();
        }
        catch (AppException e)
        {
            Console.WriteLine("Caught: {0}", e.Message);
            if (e.InnerException != null)
            {
                Console.WriteLine("Inner exception: {0}", e.InnerException);
            }
            throw new AppException("Error again in CatchInner caused by calling the ThrowInner method.", e);  // //@EXCEPTION_E@
        }
        Console.WriteLine("Complete application exception test\n");
    }
    public void ThrowInner()
    {
        throw new AppException("Exception in ThrowInner method."); // //@EXCEPTION_C@
    }
    public void CatchInner()
    {
        try
        {
            this.ThrowInner();
        }
        catch (AppException e)
        {
            throw new AppException("Error in CatchInner caused by calling the ThrowInner method.", e); // //@EXCEPTION_D@
        }
    }
}

public class MainClass
{
    public static void Main()
    { // //@MAIN@
        Test test = new Test();

        //////// "State: !Thow() && !UserUnhandled() name := '*'";
        test.TestSystemException();

        //////// "State: !Thow() && UserUnhandled() name := '*'";
        test.TestSystemException();

        //////// "State: Thow() && !UserUnhandled() name := '*'";
        test.TestSystemException();

        ////////  "State: Thow() && UserUnhandled() name := '*'";
        test.TestSystemException();

        ////////  "State: Thow() && UserUnhandled()";
        ////////  "name := System.DivideByZeroException";
        test.TestSystemException();

        ////////  "State: Thow()";
        ////////  "name := System.DivideByZeroException";
        ////////  "State: UserUnhandled()";
        ////////  "name := System.DivideByZeroException";
        test.TestSystemException();

        ////////  "State: Thow() && UserUnhandled()";
        ////////  "name := System.DivideByZeroExceptionWrong and *";
        test.TestSystemException();

        ////////  "State: Thow() && UserUnhandled()";
        ////////  "name := System.DivideByZeroExceptionWrong";
        test.TestSystemException();

        //////// "TODO:\n"
        //////// "Test for check forever waiting:\n"
        //////// "threads in NetcoreDBG (unsupported now)\n"
        //////// "Thread threadA = new Thread(new ThreadStart(test.TestSystemException));\n"
        //////// "Thread threadB = new Thread(new ThreadStart(test.TestSystemException));\n"
        //////// "threadA.Start();\n"
        //////// "threadB.Start();\n"
        //////// "threadA.Join();\n"
        //////// "threadB.Join();\n"

        //////// "Test with process exit at the end, need re-run"
        //////// "INFO: State: !Thow() && UserUnhandled() name := AppException";
        test.TestAppException();
    }
}
