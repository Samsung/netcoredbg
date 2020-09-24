cmake_minimum_required(VERSION 2.8.12.2)
project(netcoredbg)

set(CORECLR_DIR "" CACHE FILEPATH "Path to CoreCLR source directory")
set(DOTNET_DIR "" CACHE FILEPATH "Path to .NET SDK directory")
set(CORECLR_BRANCH "release/3.1" CACHE STRING "CoreCLR source branch")
set(DOTNET_CHANNEL "3.1" CACHE STRING ".NET SDK channel")
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

add_subdirectory(third_party/linenoise-ng)
add_subdirectory(src/debug/netcoredbg)

include(getgitrevisiondescription.cmake)

function(get_git_head_revision_dir _refspecvar _hashvar _dir)
    set(BACKUPDIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(CMAKE_CURRENT_SOURCE_DIR "${BACKUPDIR}${_dir}")
    get_git_head_revision(refspecvar hashvar)
    set(CMAKE_CURRENT_SOURCE_DIR "${BACKUPDIR}")
    set(${_refspecvar} "${refspecvar}" PARENT_SCOPE)
    set(${_hashvar} "${hashvar}" PARENT_SCOPE)
endfunction()

function(get_git_subject_date_revision _subjectvar _datevar)
    execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ad --date=local
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE datevar
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%s
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE subjectvar
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\\" "\\\\" subjectvar "${subjectvar}")
    string(REPLACE "\"" "\\\"" subjectvar "${subjectvar}")
    set(${_subjectvar} "${subjectvar}" PARENT_SCOPE)
    set(${_datevar} "${datevar}" PARENT_SCOPE)
endfunction()

function(get_git_subject_date_revision_dir _subjectvar _datevar _dir)
    set(BACKUPDIR "${CMAKE_SOURCE_DIR}")
    set(CMAKE_SOURCE_DIR "${BACKUPDIR}${_dir}")
    get_git_subject_date_revision(subjectvar datevar)
    set(CMAKE_SOURCE_DIR "${BACKUPDIR}")
    set(${_subjectvar} "${subjectvar}" PARENT_SCOPE)
    set(${_datevar} "${datevar}" PARENT_SCOPE)
endfunction()

get_git_head_revision(BUILD_NETCOREDBG_GIT_REFSPEC BUILD_NETCOREDBG_GIT_HEAD)
get_git_head_revision_dir(BUILD_CORECLR_GIT_REFSPEC BUILD_CORECLR_GIT_HEAD "/.coreclr")

get_git_subject_date_revision(BUILD_NETCOREDBG_GIT_SUBJECT BUILD_NETCOREDBG_GIT_DATE)
get_git_subject_date_revision_dir(BUILD_CORECLR_GIT_SUBJECT BUILD_CORECLR_GIT_DATE "/.coreclr")

execute_process(COMMAND date OUTPUT_VARIABLE BUILD_NETCOREDBG_DATE
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/debug/netcoredbg/buildinfo.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/src/debug/netcoredbg/buildinfo.cpp" @ONLY)
