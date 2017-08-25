set(netcoredbg_SRC
    main.cpp
    symbolreader.cpp
    platform.cpp
    breakpoints.cpp
    modules.cpp
    varobj.cpp
    typeprinter.cpp
    valuewalk.cpp
    valueprint.cpp
    commands.cpp
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

target_link_libraries(netcoredbg corguids dbgshim mscordaccore palrt)

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
    set(CORECLR_TOOLS ${CLR_DIR}/Tools)
    set(CORECLR_LIBS ${CORECLR_TOOLS}/dotnetcli/shared/Microsoft.NETCore.App/*)

    find_file(CORLIB NAMES System.Private.CoreLib.dll PATHS ${CLR_BIN_DIR}/IL NO_DEFAULT_PATH)
    find_file(RUNTIMELIB NAMES System.Runtime.dll PATHS ${CORECLR_LIBS} NO_DEFAULT_PATH)
    find_file(REFLECTIONLIB NAMES System.Reflection.dll PATHS ${CORECLR_LIBS} NO_DEFAULT_PATH)
    find_file(METADATALIB NAMES System.Reflection.Metadata.dll PATHS ${CORECLR_LIBS} NO_DEFAULT_PATH)
    find_file(COLLECTIONSLIB NAMES System.Collections.Immutable.dll PATHS ${CORECLR_LIBS} NO_DEFAULT_PATH)
    find_file(IOLIB NAMES System.IO.FileSystem.dll PATHS ${CORECLR_LIBS} NO_DEFAULT_PATH)

    set(SYMBOLREADER_DLL_NAME SOS.NETCore.dll)

    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${SYMBOLREADER_DLL_NAME}
      COMMAND ${CORECLR_TOOLS}/dotnetcli/dotnet
        ${CORECLR_TOOLS}/csc.exe
        /r:${CORLIB} /r:${RUNTIMELIB} /r:${REFLECTIONLIB} /r:${IOLIB} /r:${METADATALIB} /r:${COLLECTIONSLIB} /out:"${SYMBOLREADER_DLL_NAME}" /debug:portable /unsafe /target:library
        "${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.cs"

      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/SymbolReader.cs"
      COMMENT "Compiling SymbolReader.cs"
      VERBATIM
    )

    add_custom_target(symbolreader_dll ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${SYMBOLREADER_DLL_NAME})
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${SYMBOLREADER_DLL_NAME} DESTINATION ${CMAKE_INSTALL_PREFIX})
endif()

install(TARGETS netcoredbg DESTINATION ${CMAKE_INSTALL_PREFIX})
