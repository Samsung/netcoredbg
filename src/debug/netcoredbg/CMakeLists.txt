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
    expr.cpp)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Set the RPATH of sos so that it can find dependencies without needing to set LD_LIBRARY_PATH
# For more information: http://www.cmake.org/Wiki/CMake_RPATH_handling.
if (CORECLR_SET_RPATH)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  if(CLR_CMAKE_PLATFORM_DARWIN)
    set(CMAKE_INSTALL_RPATH "@loader_path")
  else()
    #set(CMAKE_INSTALL_RPATH "\$ORIGIN")
    set(CMAKE_INSTALL_RPATH "${CORECLR_SET_RPATH}")
  endif(CLR_CMAKE_PLATFORM_DARWIN)
endif()

add_definitions(-DPAL_STDCPP_COMPAT)

if(CLR_CMAKE_PLATFORM_ARCH_AMD64)
    add_definitions(-D_TARGET_AMD64_=1)
    add_definitions(-DDBG_TARGET_64BIT=1)
    add_definitions(-DDBG_TARGET_AMD64=1)
    add_definitions(-DDBG_TARGET_WIN64=1)
    add_definitions(-DBIT64)
elseif(CLR_CMAKE_PLATFORM_ARCH_I386)
    add_definitions(-D_TARGET_X86_=1)
    add_definitions(-DDBG_TARGET_32BIT=1)
    add_definitions(-DDBG_TARGET_X86=1)
elseif(CLR_CMAKE_PLATFORM_ARCH_ARM)
    add_definitions(-D_TARGET_ARM_=1)
    add_definitions(-DDBG_TARGET_32BIT=1)
    add_definitions(-DDBG_TARGET_ARM=1)
elseif(CLR_CMAKE_PLATFORM_ARCH_ARM64)
    add_definitions(-D_TARGET_ARM64_=1)
    add_definitions(-DDBG_TARGET_64BIT=1)
    add_definitions(-DDBG_TARGET_ARM64=1)
    add_definitions(-DDBG_TARGET_WIN64=1)
    add_definitions(-DBIT64)
endif()

add_executable(netcoredbg ${netcoredbg_SRC})

# CoreCLR libraries

set(CORECLR_PRODUCT ${CLR_DIR}/bin/Product/${CORECLR_PLATFORM})

add_library(mscordaccore SHARED IMPORTED)
set_target_properties(mscordaccore PROPERTIES IMPORTED_LOCATION ${CLR_BIN_DIR}/libmscordaccore.so)

add_library(dbgshim SHARED IMPORTED)
set_target_properties(dbgshim PROPERTIES IMPORTED_LOCATION ${CLR_BIN_DIR}/libdbgshim.so)

add_library(corguids STATIC IMPORTED)
set_target_properties(corguids PROPERTIES IMPORTED_LOCATION ${CLR_BIN_DIR}/lib/libcorguids.a)

add_library(palrt STATIC IMPORTED)
set_target_properties(palrt PROPERTIES IMPORTED_LOCATION ${CLR_BIN_DIR}/lib/libpalrt.a)

target_link_libraries(netcoredbg corguids dbgshim mscordaccore palrt pthread)

include_directories(${CLR_DIR}/src/inc)
include_directories(${CLR_DIR}/src/pal/inc)
include_directories(${CLR_DIR}/src/pal/inc/rt)
include_directories(${CLR_DIR}/src/pal/prebuilt/inc)
include_directories(${CLR_DIR}/src/debug/inc)
include_directories(${CLR_DIR}/src/debug/shim)
include_directories(${CLR_DIR}/src/dlls/dbgshim)
include_directories(${CLR_DIR}/src/coreclr/hosts/inc)
include_directories(${CLR_DIR}/src/ToolBox/SOS/Strike)

# SymbolReader

if (BUILD_MANAGED)
    set(SYMBOLREADER_PROJECT ${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.csproj)
    set(SYMBOLREADER_DLL_NAME SymbolReader.dll)
    set(DOTNET_BUILD_RESULT ${CMAKE_CURRENT_BINARY_DIR}/${SYMBOLREADER_DLL_NAME})

    add_custom_command(OUTPUT ${DOTNET_BUILD_RESULT}
      COMMAND dotnet build ${SYMBOLREADER_PROJECT} -o ${CMAKE_CURRENT_BINARY_DIR} /p:BaseIntermediateOutputPath=${CMAKE_CURRENT_BINARY_DIR}/obj/
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.cs" "${SYMBOLREADER_PROJECT}"
      COMMENT "Compiling ${SYMBOLREADER_DLL_NAME}"
      VERBATIM
    )

    add_custom_target(symbolreader_dll ALL DEPENDS ${DOTNET_BUILD_RESULT})
    install(FILES ${DOTNET_BUILD_RESULT} DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

install(TARGETS netcoredbg DESTINATION ${CMAKE_INSTALL_PREFIX})
