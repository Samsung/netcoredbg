using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

using Xunit;

namespace MIExampleTest2
{
    class Program
    {
        public static void testfunc()
        {
            Console.WriteLine("A breakpoint \"bp2\" is set on this line"); Label.Breakpoint("bp2");

            Label.Checkpoint("bp2_test", "finish", () => {
                Context.WasBreakpointHit(DebuggeeInfo.Breakpoints["bp2"]);
                Context.Continue();
            });
        }
    }
}
