/* vim: set ts=4 sw=4 sts=4 et ai: */

using System;
using System.Threading;
using System.Threading.Tasks;
using System.Diagnostics;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Formatters.Binary;
using System.Collections.Generic;
using System.Text;
using System.IO;

namespace Test
{
    static class Program
    {
        static public string get_caller_info()
        {
            StackFrame callStack = new StackFrame(1, true);
            return callStack.GetFileName() + ":" + (callStack.GetFileLineNumber() + 1);
        }

        static public string function(int v)
        {
            if (v != 0) Console.WriteLine("function entry");

            if (v == 0) return get_caller_info();

            v += 1;
            Console.WriteLine("function leave");
            return "";
        }

        static public int consume(string s)
        {
            return s.Length;
        }

        static public int vars(string a1, ref int a2, int a3)
        {
            ushort us = 0xcafe;
            short s1 = 0x1ace, s2 = (short)us;
            uint u = 0xfeedca75;
            int i1 = 0x1ead1e55, i2 = (int)u;
            ulong ul = 0xfee1900dc0cac01a;
            long l1 = 0x10aded1e55f001ed,  l2 = (long)ul;
            string s = "test string";
            char c = 'a';
            byte ub = 0xa5;
            sbyte b1 = 0x5a, b2 = (sbyte)ub;
            bool t = true, f = false;
            float F = 3.141592654F;
            double D = 2.71828D;
            decimal L = 123456789.123456789M;

            Console.WriteLine("us={0}, s1={1}, s2={2}, u={3}, i1={4}, i2={5}",
                                us, s1, s2, u, i1, i2);
            
            Console.WriteLine("ul={0}, l1={1}, l2={2}", ul, l1, l2);

            Console.WriteLine("s={0}, c={1}, ub={2}, b1={3}, b2={4}, t={5}, f={6}",
                                s, c, ub, b1, b2, t, f);

            Console.WriteLine("f={0}, d={1}, L={2}", F, D, L);

            Console.WriteLine("a1={0}, a2={1}, a3={2}", a1, a2, a3);

            Console.WriteLine("sv={0}", sv);

            Debug.Fail("examine variables");

            return consume(s + c + us + s1 + u + i1 + i2 + ul + l1 + l1 + ub + b1 + b2 + t + f + F + D + L);
        }


        static long sv = 0x1eadf1ee7;
        volatile static int zero = 0, one = 1;

        class Except : Exception
        {
            public Except(string msg): base(msg) {}
        }

        static void task()
        {
            System.Threading.Thread.Sleep(3000);
        }

        delegate void Delegate(string msg);

        static void delegate_impl(string msg)
        {
            Console.WriteLine("delegate: {0}", msg);
        }

        static void call_delegate(Delegate func)
        {
            func("test");
        }

        static public void consume(Object obj)
        {
            using (var devnull = new FileStream("/dev/null", FileMode.Append)) {
                new BinaryFormatter().Serialize(devnull,(obj));
            }
        }

        static void stdout_test(int len)
        {
            // using fixed seed (need reproducible result for testing)
            int send = 0;
            Random rnd = new Random(0);
            StringBuilder str = new StringBuilder();
            while (send < len)
            {
                int left = len - send;
                int size = rnd.Next(1, len / 10);
                if (size > left) size = left;
                
                str.Clear();
                string dlm = "";
                while (str.Length < size)
                {
                    str.AppendFormat("{0}{1}", dlm, size);
                    dlm = " ";
                }

                str.Length = size;
                Console.WriteLine(str);
                send += str.Length;
            }
        }

        static int fib(int x)
        {
            int y = x - 1;
            int z = x - 2;
            if (x == 0) {
                System.Diagnostics.Debugger.Break();
                return 0;
            }
            else if (x == 1)
                return 1;
            else
                return fib(y) + fib(z);
        }

        static void Main(string[] args)
        {
            Thread.CurrentThread.Name = "main (test)";

            if (args.Length == 0)
            {
                Console.WriteLine("Usage: {0} <function> [args...]", Environment.CommandLine);
                Console.WriteLine(String.Join(Environment.NewLine,
                    "Function should be one of the following:",
                    "assert      -- call Diagnostics.Debug.Assert",
                    "fail        -- call Diagnostics.Debug.Fail",
                    "break       -- call Diagnostics.Debugger.Break (deep stack)",
                    "log         -- call Diagnostics.Debugger.Log",
                    "null        -- cause ArgumentNullException",
                    "range       -- cause IndexOutOfRangeException",
                    "key         -- cause KeyNotFoundException",
                    "overflow    -- cause OverflowException",
                    "sigsegv     -- cause SIGSEGV signal (NullReferenceException)",
                    "exception   -- throw and catch test exception",
                    "task        -- run test task",
                    "delegate    -- call function via delegate",
                    "wait        -- wait for input from stdin",
                    "vars        -- test variables in debugger",
                    "function    -- test breakpoints in debugger",
                    "output      -- test stdout/stderr/debug output",
                    "print [len] -- print len bytes to stdout (1MByte default)",
                    "echo [text] -- print 'text' (or read lines from stdin and print)"
                    "sleep [sec] -- sleep (default is infinite)"
                ));
                Console.WriteLine("");
                Console.WriteLine("breakpoint location: Test::Program::function at {0}", function(0));
                Environment.Exit(0);
            }

            switch (args[0])
            {
            case "assert":
                System.Diagnostics.Debug.Assert(false, "calling Debug.Assert");
                break;

            case "fail":
                System.Diagnostics.Debug.Fail("calling Debug.Fail");
                break;

            case "break":
                fib(20);
                break;

            case "log":
                System.Diagnostics.Debugger.Log(0, null, "Test log message 1");
                System.Diagnostics.Debugger.Log(1, "category", "Test log message 2");
                break;

            case "null":
                consume(null);
                break;

            case "range": 
                {
                var v = new int[1];
                consume(v[one]);
                }
                break;

            case "key":
                {
                var v = new SortedList<int, int>();
                consume(v[zero]);
                }
                break;

            case "overflow":
                {
                int v = Int32.MaxValue;
                checked {
                    v += one;
                }
                }
                break;

            case "sigsegv":
                {
                int x = 1;
                unsafe {
                    int *i = (int*)zero;
                    x = *i;
                    consume(*i);
                }
                }
                break;

            case "exception":
                try {
                    throw (new Except("test exception"));
                }
                catch (Except e) {
                    Console.WriteLine("caught: {0}", e.Message);
                }
                break;

            case "task":
                {
                Task task = Task.Run(() => Test.Program.task());
                task.Wait();
                }
                break;

            case "delegate":
                {
                Delegate func = delegate_impl;
                call_delegate(func);
                }
                break;

            case "wait":
                Console.ReadLine();
                break;

            case "vars":
                {
                int ref_i = 42;
                vars("func-test", ref ref_i, 43);
                }
                break;

            case "function":
                function(42);
                break;

            case "output":
                Console.WriteLine("stdout output line 1");
                Console.Error.WriteLine("stderr output line 1");
                Debug.WriteLine("Debug.WriteLine message line 1");
                System.Diagnostics.Debugger.Log(0, null, "log message 1\n");
                Console.WriteLine("stdout output line 2");
                Console.Error.WriteLine("stderr output line 2");
                Debug.WriteLine("Debug.WriteLine message line 2");
                System.Diagnostics.Debugger.Log(0, null, "log message 2\n");
                break;

            case "print":
                {
                int len = 1024*1024;
                if (args.Length > 1)
                    len = Int32.Parse(args[1]);
                stdout_test(len);
                }
                break;

            case "echo":
                {
                if (args.Length > 1) {
                    Console.WriteLine(args[1]);
                    break;
                }

                try {
                    string s;
                    while (true)
                    {
                        s = Console.ReadLine();
                        if (s == null) break;
                        Console.WriteLine(s);
                    }
                }
                catch (IOException e)
                {
                    Console.Error.WriteLine(e.GetType().Name);
                }
                }
                break;

            case "sleep":
                {
                int sec = 86400;
                if (args.Length > 1)
                    sec = Int32.Parse(args[1]);
                System.Threading.Thread.Sleep(sec * 1000);
                }
                break;

            default:
                Console.Error.WriteLine("Wrong argument ({0})", args[0]);
                Environment.Exit(1);
                break;
            }
        }
    }
}

