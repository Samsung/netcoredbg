using System.Diagnostics;

using NetcoreDbgTestCore;

namespace NetcoreDbgTest
{
    static public class Label
    {
        [Conditional("TEST_LABEL")]
        public static void Breakpoint(string name)
        {
        }

        [Conditional("TEST_LABEL")]
        public static void Checkpoint(string id, string next_id, Checkpoint checkpoint)
        {
        }
    }
}
