/*
using System.IO;
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
string filename = Path.GetFileName(TestSource);
Send(String.Format("5-break-insert -f {0}:{1}", filename, Lines["PIT_STOP"]));
var r = Expect("5^done");
int b1 = r.Find("bkpt").FindInt("number");
Send("7-exec-run");
r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));
Assert.Equal(Lines["MAIN"], r.Find("frame").FindInt("line"));
Send("8-exec-continue");
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["PIT_STOP"], r.Find("frame").FindInt("line"));
Send("100-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("101-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("102-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("103-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("104-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("105-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("106-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("107-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("108-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("109-exec-continue");
r = Expect("*stopped");
Assert.Equal("exception-received", r.FindString("reason"));
Send("120-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
Send(String.Format("999-break-delete {0}", b1));
Expect("999^done");
Send("-gdb-exit");
*/

using System;
using System.Threading;

public class Test {
    public void TestSystemException()
    {
        throw new System.DivideByZeroException("Message text");
    }
    
    public void run()
    { // //@PIT_STOP@
        Test test = new Test();

        Thread thread0 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread1 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread2 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread3 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread4 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread5 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread6 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread7 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread8 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        Thread thread9 = new Thread(new System.Threading.ThreadStart(test.TestSystemException));
        
        thread0.Name = "_thread_0_";
        thread1.Name = "_thread_1_";
        thread2.Name = "_thread_2_";
        thread3.Name = "_thread_3_";
        thread4.Name = "_thread_4_";
        thread5.Name = "_thread_5_";
        thread6.Name = "_thread_6_";
        thread7.Name = "_thread_7_";
        thread8.Name = "_thread_8_";
        thread9.Name = "_thread_9_";
        
        thread0.Start();
        thread1.Start();
        thread2.Start();
        thread3.Start();
        thread4.Start();
        thread5.Start();
        thread6.Start();
        thread7.Start();
        thread8.Start();
        thread9.Start();
                
        thread9.Join();
        thread8.Join();
        thread7.Join();
        thread6.Join();
        thread5.Join();
        thread4.Join();
        thread3.Join();
        thread2.Join();
        thread1.Join();
        thread0.Join();	
    }
}

public class MainClass
{
    public static void Main()
    { // //@MAIN@
        Test test = new Test();
        //////// Empty exception filter. And we expect unhandled exception from each thread.
        //////// Line in breakpoint and exception events not available inside thread methods.
        //////// And also, this test checks additional Continue(). 
        //////// FuncEval() not checked for MI, because ExceptionInfo not used for MI.
        test.run();
    }
}
