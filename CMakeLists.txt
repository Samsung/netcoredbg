cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CLR_DIR "$ENV{HOME}/work/coreclr" CACHE FILEPATH "Path to coreclr directory")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-null-conversion")

set(CLR_CMAKE_PLATFORM_UNIX 1)

include(clrdefinitions.cmake)
include(compileoptions.cmake)

add_subdirectory(src/debug/netcoredbg)
