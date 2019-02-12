cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CORECLR_DIR "" CACHE FILEPATH "Path to CoreCLR source directory")
set(DOTNET_DIR "" CACHE FILEPATH "Path to .NET SDK directory")
set(CORECLR_BRANCH "release/2.1" CACHE STRING "CoreCLR source branch")
set(DOTNET_CHANNEL "2.1" CACHE STRING ".NET SDK channel")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_RUNTIME_DIR "" CACHE FILEPATH "Path to dbgshim library directory (at runtime)")

if (WIN32)
    set(CMAKE_CXX_STANDARD 11)
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-null-conversion")
endif()

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

# Basic checks for valid paths
if (NOT IS_DIRECTORY "${CORECLR_DIR}/src/pal")
    message(FATAL_ERROR "Invalid CoreCLR source directory")
endif()
if (NOT "${DOTNET_DIR}" STREQUAL "")
    if (NOT IS_DIRECTORY "${DOTNET_DIR}/shared/Microsoft.NETCore.App")
        message(FATAL_ERROR "Invalid .NET SDK directory")
    endif()
endif()

add_subdirectory(src/debug/netcoredbg)
