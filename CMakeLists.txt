cmake_minimum_required(VERSION 2.8.12.2)

if (BUILD_TESTING)
    enable_testing()
endif()

project(netcoredbg)

set(CORECLR_DIR "" CACHE FILEPATH "Path to CoreCLR source directory")
set(DOTNET_DIR "" CACHE FILEPATH "Path to .NET SDK directory")
set(CORECLR_BRANCH "release/8.0" CACHE STRING "CoreCLR source branch")
# After move of dbgshim from runtime to diagnostics, this sdk is used only for build of managed part.
set(DOTNET_CHANNEL "8.0" CACHE STRING ".NET SDK channel")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_DIR "" CACHE FILEPATH "Path to dbgshim library directory")

function(clr_unknown_arch)
    message(FATAL_ERROR "Only AMD64, ARM64, ARM, ARMEL, I386 and WASM are supported")
endfunction()

if (WIN32)
    # For windows it is expected that CLR_CMAKE_HOST_ARCH parameter is passed
    # to CMAKE to determine build arch.
    # If it is not passed, detect arch here.
    if ("${CLR_CMAKE_HOST_ARCH}" STREQUAL "")
        if ("${CMAKE_SIZEOF_VOID_P}" STREQUAL "4")
            set(CLR_CMAKE_HOST_ARCH x86)
        else()
            set(CLR_CMAKE_HOST_ARCH x64)
        endif()
    endif()
endif()

# Follow order as in root CoreCLR CMakeLists.txt
include(detectplatform.cmake) # from root
include(platformdefinitions.cmake) # from root
include(compileoptions.cmake) # disabled setting arm compiler flags in Tizen build
include(clrdefinitions.cmake)

include(fetchdeps.cmake)

set(CORECLR_SRC_DIR "${CORECLR_DIR}/src")
set(VERSION_PROPS_PATH "${CORECLR_DIR}/eng/Versions.props")
if (NOT EXISTS "${CORECLR_SRC_DIR}/pal")
    set(CORECLR_SRC_DIR "${CORECLR_DIR}")
    set(VERSION_PROPS_PATH "${CORECLR_DIR}/../../eng/Versions.props")
endif()

if (WIN32)
    execute_process(
            COMMAND powershell -Command "Select-Xml -Path ${VERSION_PROPS_PATH} -XPath '/Project/PropertyGroup/ProductVersion' | ForEach-Object { $_.Node.InnerXML }"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE CORECLR_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE errout
            RESULT_VARIABLE retcode)
else()
    execute_process(
            COMMAND bash -c "grep ProductVersion ${VERSION_PROPS_PATH} | sed 's,</.*,,;s,.*>,,'"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE CORECLR_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_VARIABLE errout
            RESULT_VARIABLE retcode)
endif()

if (NOT "${retcode}" STREQUAL "0" OR "${CORECLR_VERSION}" STREQUAL "")
    # For some version of runtime Versions.props file doesn't exist
    set(CORECLR_VERSION "unknown")
endif()

message("Building with ${CORECLR_VERSION} CoreCLR")

# Basic checks for valid paths
if (NOT IS_DIRECTORY "${CORECLR_SRC_DIR}/pal")
    message(FATAL_ERROR "Invalid CoreCLR source directory")
endif()
if (NOT "${DOTNET_DIR}" STREQUAL "")
    if (NOT IS_DIRECTORY "${DOTNET_DIR}/shared/Microsoft.NETCore.App")
        message(FATAL_ERROR "Invalid .NET SDK directory")
    endif()
endif()


# Determine version, VERSION macro might be passed via command line.
include(rpmbuild.cmake)
if("${VERSION}x" STREQUAL "x")
    # If not -- find *.spec file name (perhaps netcoredbg.spec) and extract version
    RPMVersion(VERSION)
endif()
 
# Get references to VCS revision for Netcoredbg itself and CoreCLR
# (this used in src/debug/netcoredbg/CMakeLists.txt)
include(vcsinfo.cmake)
VCSInfo(NETCOREDBG_VCS_INFO)
VCSInfo(CORECLR_VCS_INFO ${CORECLR_DIR})


add_subdirectory(third_party/linenoise-ng)
if (INTEROP_DEBUGGING)
    add_subdirectory(third_party/libelfin)
endif (INTEROP_DEBUGGING)
add_subdirectory(src)
