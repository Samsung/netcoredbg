# Locate dbgshim library

# application name for tizens logger
add_definitions(-DLOG_TAG="NETCOREDBG")

if (NOT DBGSHIM_DIR STREQUAL "")
    add_definitions(-DDBGSHIM_DIR="${DBGSHIM_DIR}")
endif()

# Build corguids static library from coreclr source files
if (NOT WIN32)
    add_compile_options(-Wno-extra-tokens)
endif()
add_compile_options(-D_MIDL_USE_GUIDDEF_)
file(GLOB CORGUIDS_SOURCES "${CORECLR_SRC_DIR}/pal/prebuilt/idl/*_i.cpp")
add_library(corguids STATIC ${CORGUIDS_SOURCES})
if (NOT WIN32)
    target_compile_options(corguids PRIVATE -Wno-unused-parameter)
    target_include_directories(corguids PRIVATE ${CORECLR_SRC_DIR}/pal/inc ${CORECLR_SRC_DIR}/pal/inc/rt)
    target_include_directories(corguids PRIVATE ${CORECLR_SRC_DIR}/inc ${CORECLR_SRC_DIR}/debug/inc)
endif()

# Include coreclr headers

if (NOT WIN32)
    include_directories(${CORECLR_SRC_DIR}/pal/prebuilt/inc)
    include_directories(${CMAKE_CURRENT_SOURCE_DIR}/coreclr)
else()
    include_directories(${CORECLR_SRC_DIR}/inc ${CORECLR_SRC_DIR}/debug/inc)
endif()
include_directories(${CORECLR_SRC_DIR})
include_directories(${CORECLR_SRC_DIR}/debug/shim)
include_directories(${CORECLR_SRC_DIR}/dlls/dbgshim)
# for CoreCLR <= 3.x
include_directories(${CORECLR_SRC_DIR}/coreclr/hosts/inc)
# for dotnet-runtime (> 3.x)
include_directories(${CORECLR_SRC_DIR}/hosts/inc)

# Build native part of the debugger

include_directories(${PROJECT_SOURCE_DIR}/third_party)
include_directories(${PROJECT_SOURCE_DIR}/third_party/linenoise-ng/include)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR}/generated)
include_directories(${PROJECT_SOURCE_DIR}/src/windows ${PROJECT_SOURCE_DIR}/src/unix)

# Generate error messages from corerror.xml

set(ERRORMESSAGE_DLL_NAME generrmsg/bin/generrmsg.dll)
find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generrmsg/generrmsg.csproj"
  COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NEW_LOCAL_SEARCH_FILE_ONLY=1 ${DOTNETCLI} new console --force -n generrmsg
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Creating new dotnet project"
  VERBATIM
)  

add_custom_command(
  OUTPUT ${ERRORMESSAGE_DLL_NAME}
  COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/tools/generrmsg/GenErrMsg.cs Program.cs
  COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_SOURCE_DIR}/tools/generrmsg/nuget.xml nuget.config
  COMMAND ${DOTNETCLI} build -o bin/
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generrmsg"
  DEPENDS "${PROJECT_SOURCE_DIR}/tools/generrmsg/GenErrMsg.cs" "${CMAKE_CURRENT_BINARY_DIR}/generrmsg/generrmsg.csproj"
  COMMENT "Compiling ${ERRORMESSAGE_DLL_NAME}"
  VERBATIM
)

add_custom_command(
  OUTPUT errormessage.cpp
  COMMAND ${DOTNETCLI} generrmsg/bin/generrmsg.dll ${CORECLR_SRC_DIR}/inc/corerror.xml ${CMAKE_CURRENT_BINARY_DIR}/errormessage.cpp ${CMAKE_CURRENT_BINARY_DIR}/errormessage.h
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS "${CORECLR_SRC_DIR}/inc/corerror.xml" ${ERRORMESSAGE_DLL_NAME}
  COMMENT "Extracting ${CORECLR_SRC_DIR}/inc/corerror.xml"
  VERBATIM
)

set(netcoredbg_SRC
    debugger/breakpoint_break.cpp
    debugger/breakpoint_entry.cpp
    debugger/breakpoint_hotreload.cpp
    debugger/breakpoints_exception.cpp
    debugger/breakpoints_func.cpp
    debugger/breakpoints_line.cpp
    debugger/breakpoints.cpp
    debugger/breakpointutils.cpp
    debugger/callbacksqueue.cpp
    debugger/evalhelpers.cpp
    debugger/evalstackmachine.cpp
    debugger/evaluator.cpp
    debugger/evalwaiter.cpp
    debugger/evalutils.cpp
    debugger/frames.cpp
    debugger/hotreloadhelpers.cpp
    debugger/managedcallback.cpp
    debugger/manageddebugger.cpp
    debugger/threads.cpp
    debugger/stepper_async.cpp
    debugger/stepper_simple.cpp
    debugger/steppers.cpp
    debugger/valueprint.cpp
    debugger/variables.cpp
    debugger/waitpid.cpp
    interfaces/types.cpp
    managed/interop.cpp
    metadata/attributes.cpp
    metadata/async_info.cpp
    metadata/jmc.cpp
    metadata/modules.cpp
    metadata/modules_app_update.cpp
    metadata/modules_sources.cpp
    metadata/typeprinter.cpp
    protocols/cliprotocol.cpp
    protocols/escaped_string.cpp
    protocols/protocol_utils.cpp
    protocols/miprotocol.cpp
    protocols/tokenizer.cpp
    protocols/vscodeprotocol.cpp
    protocols/sourcestorage.cpp
    utils/utf.cpp
    errormessage.cpp
    main.cpp
    buildinfo.cpp
    utils/dynlibs_unix.cpp
    utils/dynlibs_win32.cpp
    utils/filesystem.cpp
    utils/filesystem_unix.cpp
    utils/filesystem_win32.cpp
    utils/ioredirect.cpp
    utils/iosystem_unix.cpp
    utils/iosystem_win32.cpp
    utils/interop_unix.cpp
    utils/interop_win32.cpp
    utils/platform_unix.cpp
    utils/platform_win32.cpp
    utils/streams.cpp
    )

set(CMAKE_INCLUDE_CURRENT_DIR OFF)

if (NOT WIN32)
    add_definitions(-DPAL_STDCPP_COMPAT)
endif()

if (WIN32)
    # fix issue with std::numeric_limits<T>::max() and std::max()
    add_definitions(-DNOMINMAX)
endif()

if (NOT CLR_CMAKE_TARGET_TIZEN_LINUX)
    list(APPEND netcoredbg_SRC utils/logger.cpp)
endif()

if (INTEROP_DEBUGGING)
    list(APPEND netcoredbg_SRC
            debugger/breakpoint_interop_rendezvous.cpp
            debugger/breakpoints_interop.cpp
            debugger/breakpoints_interop_line.cpp
            debugger/interop_brk_helpers.cpp
            debugger/interop_debugging.cpp
            debugger/interop_mem_helpers.cpp
            debugger/interop_ptrace_helpers.cpp
            debugger/interop_unwind.cpp
            debugger/sigaction.cpp
            metadata/interop_libraries.cpp
        )
endif (INTEROP_DEBUGGING)

# special threatment of buildinfo.cpp -- this file rebuilds every time,
# whatever it changed or not (because it contains version information and
# date/time of the build).
cmake_host_system_information(RESULT fqdn QUERY FQDN)
string(CONCAT buildinfo_flags
    " " -DVERSION=${VERSION}
    " " -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
    " " -DNETCOREDBG_VCS_INFO=${NETCOREDBG_VCS_INFO}
    " " -DCORECLR_VCS_INFO=${CORECLR_VCS_INFO}
    " " -DOS_NAME=${CMAKE_SYSTEM_NAME}
    " " -DCPU_ARCH=${CLR_CMAKE_TARGET_ARCH}
    " " -DHOSTNAME=${fqdn}
)
set_source_files_properties(buildinfo.cpp PROPERTIES COMPILE_FLAGS "${buildinfo_flags}")
add_custom_target(buildinfo ALL COMMAND
    ${CMAKE_COMMAND} -E touch "${CMAKE_SOURCE_DIR}/src/buildinfo.cpp")


add_executable(netcoredbg ${netcoredbg_SRC})

if (WIN32)
    target_link_libraries(netcoredbg corguids wsock32 ws2_32 linenoise)
else()
    target_link_libraries(netcoredbg corguids dl pthread linenoise)
endif()

if (NCDB_DOTNET_STARTUP_HOOK)
    add_definitions(-DNCDB_DOTNET_STARTUP_HOOK="${NCDB_DOTNET_STARTUP_HOOK}")
endif (NCDB_DOTNET_STARTUP_HOOK)

if (INTEROP_DEBUGGING)
    if (NOT CLR_CMAKE_PLATFORM_LINUX)
        message(FATAL_ERROR "Interop debugging feature not implemented for this OS.")
    endif()

    add_definitions(-DINTEROP_DEBUGGING)

    if (CLR_CMAKE_PLATFORM_UNIX_AMD64)
        add_definitions(-DDEBUGGER_UNIX_AMD64)
    elseif (CLR_CMAKE_PLATFORM_UNIX_X86)
        add_definitions(-DDEBUGGER_UNIX_X86)
    elseif (CLR_CMAKE_PLATFORM_UNIX_ARM64)
        add_definitions(-DDEBUGGER_UNIX_ARM64)
    elseif (CLR_CMAKE_PLATFORM_UNIX_ARM)
        add_definitions(-DDEBUGGER_UNIX_ARM)
    else()
        message(FATAL_ERROR "Interop debugging feature not implemented for this CPU architecture.")
    endif()

    target_link_libraries(netcoredbg elf++ dwarf++)
    include_directories(${PROJECT_SOURCE_DIR}/third_party/libelfin/elf ${PROJECT_SOURCE_DIR}/third_party/libelfin/dwarf)


    # libunwind
    find_path(LIBUNWIND_INCLUDE_DIRS NAMES libunwind.h)
    find_library(LIBUNWIND_LIBRARY unwind)
    find_library(LIBUNWIND_PTRACE_LIBRARY unwind-ptrace)
    if (CLR_CMAKE_PLATFORM_UNIX_AMD64)
        find_library(LIBUNWIND_LIBRARY_ARCH unwind-x86_64)
    elseif (CLR_CMAKE_PLATFORM_UNIX_X86)
        find_library(LIBUNWIND_LIBRARY_ARCH unwind-x86)
    elseif (CLR_CMAKE_PLATFORM_UNIX_ARM64)
        find_library(LIBUNWIND_LIBRARY_ARCH unwind-aarch64)
    elseif (CLR_CMAKE_PLATFORM_UNIX_ARM)
        find_library(LIBUNWIND_LIBRARY_ARCH unwind-arm)
    endif()

    if (${LIBUNWIND_INCLUDE_DIRS} STREQUAL "LIBUNWIND_INCLUDE_DIRS-NOTFOUND")
        message(FATAL_ERROR "Libunwind headers not found")
    endif()

    if (${LIBUNWIND_LIBRARY} STREQUAL "LIBUNWIND_LIBRARY-NOTFOUND")
        message(FATAL_ERROR "Libunwind lib not found")
    endif()

    if (${LIBUNWIND_PTRACE_LIBRARY} STREQUAL "LIBUNWIND_PTRACE_LIBRARY-NOTFOUND")
        message(FATAL_ERROR "Libunwind-ptrace libs not found")
    endif()

    if (${LIBUNWIND_LIBRARY_ARCH} STREQUAL "LIBUNWIND_LIBRARY_ARCH-NOTFOUND")
        message(FATAL_ERROR "Libunwind libs for this arch not found")
    endif()

    target_link_libraries(netcoredbg ${LIBUNWIND_LIBRARY} ${LIBUNWIND_PTRACE_LIBRARY} ${LIBUNWIND_LIBRARY_ARCH})
    include_directories(${LIBUNWIND_INCLUDE_DIRS})
endif (INTEROP_DEBUGGING)

if (CLR_CMAKE_TARGET_TIZEN_LINUX)
    add_definitions(-DDEBUGGER_FOR_TIZEN)
    target_link_libraries(netcoredbg dlog)
endif (CLR_CMAKE_TARGET_TIZEN_LINUX)

add_custom_command(
  TARGET netcoredbg
  PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E touch_nocreate ${ERRORMESSAGE_DLL_NAME}
  COMMAND ${CMAKE_COMMAND} -E remove -f obj/project.assets.json
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

install(TARGETS netcoredbg DESTINATION ${CMAKE_INSTALL_PREFIX})

# Build managed part of the debugger (ManagedPart.dll)

if (BUILD_MANAGED)
    set(MANAGEDPART_PROJECT ${CMAKE_CURRENT_SOURCE_DIR}/managed/ManagedPart.csproj)
    set(MANAGEDPART_DLL_NAME ManagedPart.dll)
    set(DOTNET_BUILD_RESULT ${CMAKE_CURRENT_BINARY_DIR}/${MANAGEDPART_DLL_NAME})

    find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(MANAGEDPART_BUILD_TYPE "Debug")
    else() # CMAKE_BUILD_TYPE not set or Release, RelWithDebInfo, MinSizeRel, ...
        set(MANAGEDPART_BUILD_TYPE "Release")
    endif()

    set(USE_DBGSHIM_DEPENDENCY "")
    if (DBGSHIM_DIR STREQUAL "")
        set(USE_DBGSHIM_DEPENDENCY "/p:UseDbgShimDependency=true")
    endif()

    if (NOT RID_NAME)
        if (CLR_CMAKE_PLATFORM_UNIX)
            if (CLR_CMAKE_PLATFORM_DARWIN)
                set(RID_NAME "osx")
            elseif (EXISTS "/etc/alpine-release") # Alpine use musl only.
                set(RID_NAME "linux-musl")
            else()
                set(RID_NAME "linux")
            endif()
        elseif (WIN32)
            set(RID_NAME "win")
        else()
            message(FATAL_ERROR "Unsupported platform")
        endif()
    endif() # NOT RID_NAME

    add_custom_command(OUTPUT ${DOTNET_BUILD_RESULT}
      COMMAND ${DOTNETCLI} publish ${MANAGEDPART_PROJECT} -r ${RID_NAME}-${CLR_CMAKE_TARGET_ARCH} --self-contained -c ${MANAGEDPART_BUILD_TYPE} -o ${CMAKE_CURRENT_BINARY_DIR} /p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/obj/ /p:BaseOutputPath=${CMAKE_CURRENT_BINARY_DIR}/bin/ ${USE_DBGSHIM_DEPENDENCY}
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/managed/*.cs" "${MANAGEDPART_PROJECT}"
      COMMENT "Compiling ${MANAGEDPART_DLL_NAME}"
      VERBATIM
    )

    add_custom_target(managedpart_dll ALL DEPENDS ${DOTNET_BUILD_RESULT})

    # Copy dlls
    set(ROSLYN_DLLS
        Microsoft.CodeAnalysis.dll
        Microsoft.CodeAnalysis.CSharp.dll
        Microsoft.CodeAnalysis.Scripting.dll
        Microsoft.CodeAnalysis.CSharp.Scripting.dll)

    set(DLLS_TO_DEPLOY ${DOTNET_BUILD_RESULT})
    foreach(ITEM ${ROSLYN_DLLS})
        list(APPEND DLLS_TO_DEPLOY "${CMAKE_CURRENT_BINARY_DIR}/${ITEM}")
    endforeach()

    install(FILES ${DLLS_TO_DEPLOY} DESTINATION ${CMAKE_INSTALL_PREFIX})

    if (DBGSHIM_DIR STREQUAL "")
        if (CLR_CMAKE_PLATFORM_UNIX)
            if (CLR_CMAKE_PLATFORM_DARWIN)
                set(DBGSHIM_LIB_NAME "libdbgshim.dylib")
            else()
                set(DBGSHIM_LIB_NAME "libdbgshim.so")
            endif()
        elseif (WIN32)
            set(DBGSHIM_LIB_NAME "dbgshim.dll")
        else()
            message(FATAL_ERROR "Unsupported platform")
        endif()

        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${DBGSHIM_LIB_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})
    endif()
endif()

# Build Hot Reload managed dll (ncdbhook.dll)

if (BUILD_MANAGED AND NCDB_DOTNET_STARTUP_HOOK)
    set(NCDBHOOK_PROJECT ${CMAKE_CURRENT_SOURCE_DIR}/ncdbhook/ncdbhook.csproj)
    set(NCDBHOOK_DLL_NAME ncdbhook.dll)
    set(NCDBHOOK_DOTNET_BUILD_RESULT ${CMAKE_CURRENT_BINARY_DIR}/${NCDBHOOK_DLL_NAME})
    set(NCDB_HOOK_TargetFramework "net6.0")

    find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

    # Build Debug only, code must be not optimized.
    add_custom_command(OUTPUT ${NCDBHOOK_DOTNET_BUILD_RESULT}
      COMMAND NCDB_HOOK_TargetFramework=${NCDB_HOOK_TargetFramework} ${DOTNETCLI} publish ${NCDBHOOK_PROJECT} -c Debug -o ${CMAKE_CURRENT_BINARY_DIR} /p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/obj/ /p:BaseOutputPath=${CMAKE_CURRENT_BINARY_DIR}/bin/
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ncdbhook/*.cs" "${NCDBHOOK_PROJECT}"
      COMMENT "Compiling ${NCDBHOOK_DLL_NAME}"
      VERBATIM
    )

    add_custom_target(ncdbhook_dll ALL DEPENDS ${NCDBHOOK_DOTNET_BUILD_RESULT})

    install(FILES ${NCDBHOOK_DOTNET_BUILD_RESULT} DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

# documentation
option(BUILD_DOC "Build documentation" OFF)
if (BUILD_DOC)
find_package(Doxygen)
if (DOXYGEN_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/.doxyfile ${CMAKE_CURRENT_BINARY_DIR}/doxyfile)
    add_custom_target(doxygen ALL
        COMMAND ${DOXYGEN_EXECUTABLE} -s ${CMAKE_CURRENT_BINARY_DIR}/doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating doxygen documentation..."
        VERBATIM)
else()
    message("Doxygen need to be installed to generate documentation")
endif()
endif()

# unit tests
if (BUILD_TESTING)
    add_subdirectory(unittests)
endif()

