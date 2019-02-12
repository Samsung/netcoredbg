# Locate dbgshim library

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
include_directories(${CORECLR_DIR}/src/coreclr/hosts/inc)
include_directories(${CORECLR_DIR}/src/ToolBox/SOS/Strike)

# Build native part of the debugger

set(netcoredbg_SRC
    main.cpp
    manageddebugger.cpp
    symbolreader.cpp
    platform.cpp
    breakpoints.cpp
    modules.cpp
    variables.cpp
    typeprinter.cpp
    valuewalk.cpp
    valueprint.cpp
    miprotocol.cpp
    vscodeprotocol.cpp
    frames.cpp
    jmc.cpp
    cputil.cpp
    expr.cpp
    valuewrite.cpp
    logger.cpp)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if (NOT WIN32)
    add_definitions(-DPAL_STDCPP_COMPAT)
endif()

add_executable(netcoredbg ${netcoredbg_SRC})

if (WIN32)
    target_link_libraries(netcoredbg corguids wsock32 ws2_32)
else()
    target_link_libraries(netcoredbg corguids dl pthread)
endif()

if (CLR_CMAKE_TARGET_TIZEN_LINUX)
    add_definitions(-DDEBUGGER_FOR_TIZEN)
    target_link_libraries(netcoredbg dlog)
endif (CLR_CMAKE_TARGET_TIZEN_LINUX)

install(TARGETS netcoredbg DESTINATION ${CMAKE_INSTALL_PREFIX})
if (DBGSHIM_LOCATION)
    install(FILES ${DBGSHIM_LOCATION} DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

# Build managed part of the debugger (SymbolReader.dll)

if (BUILD_MANAGED)
    set(SYMBOLREADER_PROJECT ${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.csproj)
    set(SYMBOLREADER_DLL_NAME SymbolReader.dll)
    set(DOTNET_BUILD_RESULT ${CMAKE_CURRENT_BINARY_DIR}/${SYMBOLREADER_DLL_NAME})

    find_program(DOTNETCLI dotnet PATHS "${DOTNET_DIR}" ENV PATH NO_DEFAULT_PATH)

    add_custom_command(OUTPUT ${DOTNET_BUILD_RESULT}
      COMMAND ${DOTNETCLI} publish ${SYMBOLREADER_PROJECT} -o ${CMAKE_CURRENT_BINARY_DIR} /p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/obj/
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.cs" "${SYMBOLREADER_PROJECT}"
      COMMENT "Compiling ${SYMBOLREADER_DLL_NAME}"
      VERBATIM
    )

    add_custom_target(symbolreader_dll ALL DEPENDS ${DOTNET_BUILD_RESULT})

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
