using System;
using System.IO;

using NetcoreDbgTest;
using NetcoreDbgTest.MI;
using NetcoreDbgTest.Script;

namespace MIExampleTest2
{
    class Program
    {
        public static void testfunc()
        {
            Console.WriteLine("A breakpoint \"bp2\" is set on this line"); Label.Breakpoint("bp2");

            Label.Checkpoint("bp2_test", "finish", (Object context) => {
                Context Context = (Context)context;
                Context.WasBreakpointHit(@"__FILE__:__LINE__", "bp2");
                Context.Continue(@"__FILE__:__LINE__");
            });
        }
    }
}
