cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CLR_DIR "${CMAKE_SOURCE_DIR}/../coreclr" CACHE FILEPATH "Path to coreclr directory")
set(CLR_BIN_DIR "" CACHE FILEPATH "Path to coreclr bin directory")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_INSTALL ON CACHE BOOL "Install libdbgshim")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-null-conversion")

function(clr_unknown_arch)
    message(FATAL_ERROR "Only AMD64, ARM64, ARM, ARMEL, I386 and WASM are supported")
endfunction()

# Follow order as in root CoreCLR CMakeLists.txt
include(detectplatform.cmake) # from root
include(platformdefinitions.cmake) # from root
include(compileoptions.cmake) # disabled setting arm compiler flags in Tizen build
include(clrdefinitions.cmake)

if (NOT IS_DIRECTORY "${CLR_DIR}")
    message(FATAL_ERROR "Unable to find CoreCLR directory")
endif()

set(CLR_CONFIGURATION_TYPES "Debug;Checked;Release;RelWithDebInfo" CACHE STRING "")

if ("${CLR_BIN_DIR}" STREQUAL "")
    if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
        set(TARGET_OS_NAME "OSX")
    elseif (CMAKE_SYSTEM_NAME STREQUAL Linux)
        set(TARGET_OS_NAME "Linux")
    endif()

    # TODO: Search for CMAKE_BUILD_TYPE configuration first
    # TODO: CLR_CMAKE_TARGET_ARCH may be armel
    foreach(CONFIG IN LISTS CLR_CONFIGURATION_TYPES)
        set(CLR_BIN_DIR ${CLR_DIR}/bin/Product/${TARGET_OS_NAME}.${CLR_CMAKE_TARGET_ARCH}.${CONFIG})
        if (IS_DIRECTORY "${CLR_BIN_DIR}")
            break()
        endif()
    endforeach()

    if (IS_DIRECTORY "${CLR_BIN_DIR}")
        message(STATUS "CoreCLR bin directory: " ${CLR_BIN_DIR})
    else()
        message(FATAL_ERROR "Unable to find CoreCLR bin directory")
    endif()
endif()

add_subdirectory(src/debug/netcoredbg)
