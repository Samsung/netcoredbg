using NetcoreDbgTestCore;

namespace NetcoreDbgTest
{
    namespace VSCode
    {
        class VSCodeLineBreakpoint : LineBreakpoint
        {
            public VSCodeLineBreakpoint(string name, string srcName, int lineNum)
                : base(name, srcName, lineNum, ProtocolType.VSCode)
            {
            }

            public override string ToString()
            {
                return System.String.Format("{0}:{1}", FileName, NumLine);
            }
        }
    }
}
