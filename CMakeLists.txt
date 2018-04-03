cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CLR_DIR "$ENV{HOME}/work/coreclr" CACHE FILEPATH "Path to coreclr directory")
set(CLR_BIN_DIR "${CLR_DIR}" CACHE FILEPATH "Path to coreclr bin directory")
set(BUILD_MANAGED ON CACHE BOOL "Build managed part")
set(DBGSHIM_INSTALL ON CACHE BOOL "Install libdbgshim")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wno-null-conversion")

include(clrdefinitions.cmake)
include(compileoptions.cmake)

add_subdirectory(src/debug/netcoredbg)
