cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CLR_DIR "${CMAKE_SOURCE_DIR}/../coreclr" CACHE FILEPATH "Path to coreclr directory")
set(CLR_BIN_DIR "" CACHE FILEPATH "Path to coreclr bin directory")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_INSTALL ON CACHE BOOL "Install libdbgshim")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-null-conversion")

include(clrdefinitions.cmake)
include(compileoptions.cmake)

if (NOT IS_DIRECTORY "${CLR_DIR}")
    message(FATAL_ERROR "Unable to find CoreCLR directory")
endif()

if ("${CLR_BIN_DIR}" STREQUAL "")
    if (CMAKE_SYSTEM_NAME STREQUAL Darwin)
        set(TARGET_OS_NAME "OSX")
    elseif (CMAKE_SYSTEM_NAME STREQUAL Linux)
        set(TARGET_OS_NAME "Linux")
    endif()
    # TODO: Search for CMAKE_BUILD_TYPE configuration first
    # TODO: CLR_CMAKE_TARGET_ARCH may be armel
    set(CLR_BIN_DIR ${CLR_DIR}/bin/Product/${TARGET_OS_NAME}.${CLR_CMAKE_TARGET_ARCH}.Debug)
    if (NOT IS_DIRECTORY "${CLR_BIN_DIR}")
        set(CLR_BIN_DIR ${CLR_DIR}/bin/Product/${TARGET_OS_NAME}.${CLR_CMAKE_TARGET_ARCH}.Release)
    endif()
    if (NOT IS_DIRECTORY "${CLR_BIN_DIR}")
        set(CLR_BIN_DIR ${CLR_DIR}/bin/Product/${TARGET_OS_NAME}.${CLR_CMAKE_TARGET_ARCH}.Checked)
    endif()

    if (IS_DIRECTORY "${CLR_BIN_DIR}")
        message(STATUS "CoreCLR bin directory: " ${CLR_BIN_DIR})
    else()
        message(FATAL_ERROR "Unable to find CoreCLR bin directory")
    endif()
endif()

add_subdirectory(src/debug/netcoredbg)
