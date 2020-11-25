#ifndef _NETCOREDBG_BUILDINFO_CONSTANT
#define _NETCOREDBG_BUILDINFO_CONSTANT

namespace netcoredbg
{

// We need unique value for each binary netcoredbg files
struct BUILDINFO
{
    static const std::string BUILD_NETCOREDBG_GIT_REFSPEC;
    static const std::string BUILD_NETCOREDBG_GIT_HEAD;
    static const std::string BUILD_NETCOREDBG_GIT_SUBJECT;
    static const std::string BUILD_NETCOREDBG_GIT_DATE;

    static const std::string BUILD_CORECLR_GIT_REFSPEC;
    static const std::string BUILD_CORECLR_GIT_HEAD;
    static const std::string BUILD_CORECLR_GIT_SUBJECT;
    static const std::string BUILD_CORECLR_GIT_DATE;

    static const std::string BUILD_NETCOREDBG_DATE;        // Building date and time
    static const std::string CMAKE_SYSTEM_NAME;            // Value: string Windows
    static const std::string CLR_CMAKE_TARGET_ARCH;        // Value: string x64
	
    // TODO:
    // Also available some othre values from.cmake
    // For example # CLR_CMAKE_TARGET_TIZEN_LINUX // Value: int 1/0
    //
    // And we can make the self hash value from sources
    //  we take sha for each source file in project and produce summ
    //
    // And we can check git directory on any local changes
};

} // namespace netcoredbg

#endif // _NETCOREDBG_BUILDINFO_CONSTANT
