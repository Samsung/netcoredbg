namespace VSCodeTestStaticFieldsLib
{
    // Base type in other module for VSCodeTestStaticFields test, no static constructor and static fields are not used by
    // program code.
    public class NoCctorOtherModuleBase
    {
        public static int lib_s_int;
        public static string lib_s_string;
    }
}
