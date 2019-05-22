using System;

namespace NetcoreDbgTest
{
    public static class Logger
    {
        public static void LogLine(string line)
        {
            Console.WriteLine(line);
        }

        public static void LogError(string error)
        {
            Console.Error.WriteLine(error);
        }
    }
}
