# Locate dbgshim library

# application name for tizens logger
add_definitions(-DLOG_TAG="NETCOREDBG")

if (NOT DBGSHIM_RUNTIME_DIR STREQUAL "")
    add_definitions(-DDBGSHIM_RUNTIME_DIR="${DBGSHIM_RUNTIME_DIR}")
else()
    find_file(DBGSHIM_LOCATION NAMES dbgshim.dll libdbgshim.so libdbgshim.dylib PATHS "${DOTNET_DIR}/shared/Microsoft.NETCore.App/*" NO_DEFAULT_PATH)
endif()

# Build corguids static library from coreclr source files

if (NOT WIN32)
    add_compile_options(-Wno-extra-tokens)
endif()
add_compile_options(-D_MIDL_USE_GUIDDEF_)
file(GLOB CORGUIDS_SOURCES "${CORECLR_DIR}/src/pal/prebuilt/idl/*_i.cpp")
add_library(corguids STATIC ${CORGUIDS_SOURCES})
if (NOT WIN32)
    target_compile_options(corguids PRIVATE -Wno-unused-parameter)
endif()

# Include coreclr headers

if (NOT WIN32)
    include_directories(${CORECLR_DIR}/src/pal/inc)
    include_directories(${CORECLR_DIR}/src/pal/inc/rt)
    include_directories(${CORECLR_DIR}/src/pal/prebuilt/inc)
endif()
include_directories(${CORECLR_DIR}/src/inc)
include_directories(${CORECLR_DIR}/src/debug/inc)
include_directories(${CORECLR_DIR}/src/debug/shim)
include_directories(${CORECLR_DIR}/src/dlls/dbgshim)
# for CoreCLR <= 3.x
include_directories(${CORECLR_DIR}/src/coreclr/hosts/inc)
# for dotnet-runtime (> 3.x)
include_directories(${CORECLR_DIR}/src/hosts/inc)

# Build native part of the debugger

include_directories(${PROJECT_SOURCE_DIR}/third_party)
include_directories(${PROJECT_SOURCE_DIR}/third_party/linenoise-ng/include)

# Generate error messages from corerror.xml

set(ERRORMESSAGE_DLL_NAME generrmsg/bin/generrmsg.dll)
find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

add_custom_command(
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generrmsg/generrmsg.csproj"
  COMMAND ${CMAKE_COMMAND} -E env DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NEW_LOCAL_SEARCH_FILE_ONLY=1 ${DOTNETCLI} new console -n generrmsg
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
  COMMAND ${DOTNETCLI} generrmsg/bin/generrmsg.dll ${CORECLR_DIR}/src/inc/corerror.xml ${CMAKE_CURRENT_BINARY_DIR}/errormessage.cpp ${CMAKE_CURRENT_BINARY_DIR}/errormessage.h
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS "${CORECLR_DIR}/src/inc/corerror.xml" ${ERRORMESSAGE_DLL_NAME}
  COMMENT "Extracting ${CORECLR_DIR}/src/inc/corerror.xml"
  VERBATIM
)

set(netcoredbg_SRC
    debugger/breakpoints.cpp
    debugger/expr.cpp
    debugger/frames.cpp
    debugger/manageddebugger.cpp
    debugger/valueprint.cpp
    debugger/valuewalk.cpp
    debugger/valuewrite.cpp
    debugger/variables.cpp
    managed/interop.cpp
    metadata/jmc.cpp
    metadata/modules.cpp
    metadata/typeprinter.cpp
    protocols/cliprotocol.cpp
    protocols/iprotocol.cpp
    protocols/miprotocol.cpp
    protocols/protocol.cpp
    protocols/tokenizer.cpp
    protocols/vscodeprotocol.cpp
    utils/utf.cpp
    buildinfo.cpp
    errormessage.cpp
    main.cpp
    platform.cpp
    )

set(CMAKE_INCLUDE_CURRENT_DIR ON)

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


add_executable(netcoredbg ${netcoredbg_SRC})

if (WIN32)
    target_link_libraries(netcoredbg corguids wsock32 ws2_32 linenoise)
else()
    target_link_libraries(netcoredbg corguids dl pthread linenoise)
endif()

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
if (DBGSHIM_LOCATION)
    install(FILES ${DBGSHIM_LOCATION} DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

# Build managed part of the debugger (ManagedPart.dll)

if (BUILD_MANAGED)
    set(MANAGEDPART_PROJECT ${CMAKE_CURRENT_SOURCE_DIR}/managed/ManagedPart.csproj)
    set(MANAGEDPART_DLL_NAME ManagedPart.dll)
    set(DOTNET_BUILD_RESULT ${CMAKE_CURRENT_BINARY_DIR}/${MANAGEDPART_DLL_NAME})

    find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

    add_custom_command(OUTPUT ${DOTNET_BUILD_RESULT}
      COMMAND ${DOTNETCLI} publish ${MANAGEDPART_PROJECT} -o ${CMAKE_CURRENT_BINARY_DIR} /p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/obj/ /p:BaseOutputPath=${CMAKE_CURRENT_BINARY_DIR}/bin/
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/managed/SymbolReader.cs" "${MANAGEDPART_PROJECT}"
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
endif()
