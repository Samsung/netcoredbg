using NetcoreDbgTestCore;

namespace NetcoreDbgTest
{
    namespace MI
    {
        class MILineBreakpoint : LineBreakpoint
        {
            public MILineBreakpoint(string name, string srcName, int lineNum)
                : base(name, srcName, lineNum, ProtocolType.MI)
            {
            }

            public override string ToString()
            {
                return System.String.Format("{0}:{1}", FileName, NumLine);
            }
        }
    }
}
