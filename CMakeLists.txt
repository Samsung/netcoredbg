cmake_minimum_required(VERSION 2.8.12.2)

if (BUILD_TESTING)
    enable_testing()
endif()

project(netcoredbg)

set(CORECLR_DIR "" CACHE FILEPATH "Path to CoreCLR source directory")
set(DOTNET_DIR "" CACHE FILEPATH "Path to .NET SDK directory")
# Note, since 5.0 src are broken and will not build netcoredbg without patch, stay with 3.1 for now.
set(CORECLR_BRANCH "release/3.1" CACHE STRING "CoreCLR source branch")
# Note, we must use latest runtime, since we need copy latest dbgshim in order to work with all runtime versions.
# https://github.com/dotnet/runtime/pull/44748#issuecomment-729268650
set(DOTNET_CHANNEL "5.0" CACHE STRING ".NET SDK channel")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_RUNTIME_DIR "" CACHE FILEPATH "Path to dbgshim library directory (at runtime)")

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
if (NOT EXISTS "${CORECLR_SRC_DIR}/pal")
  set(CORECLR_SRC_DIR "${CORECLR_DIR}")
endif()

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
add_subdirectory(src)
