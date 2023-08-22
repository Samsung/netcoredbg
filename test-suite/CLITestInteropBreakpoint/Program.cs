using System;
using System.Runtime.InteropServices;

namespace CLITestInteropBreakpoint
{
    class Program
    {
        [DllImport("./libtest_breakpoint.so")]
        public static extern void native_method();

        static void Main(string[] args)
        {
            Console.WriteLine("<stdout_marker>Managed: Start");

            native_method(); // BREAK1

            Console.WriteLine("<stdout_marker>Managed: End");
        }
    }
}
