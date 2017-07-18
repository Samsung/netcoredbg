project(netcoredbg)

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
    jmc.cpp)

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

add_compile_options(-fPIE)
add_definitions(-DPAL_STDCPP_COMPAT)

include_directories(${CLR_DIR}/src/inc)
include_directories(${CLR_DIR}/src/pal/inc)
include_directories(${CLR_DIR}/src/pal/inc/rt)
include_directories(${CLR_DIR}/src/pal/prebuilt/inc)
include_directories(${CLR_DIR}/src/debug/inc)
include_directories(${CLR_DIR}/src/debug/shim)
include_directories(${CLR_DIR}/src/dlls/dbgshim)
include_directories(${CLR_DIR}/src/coreclr/hosts/inc)

include_directories(${CLR_DIR}/src/ToolBox/SOS/Strike)

target_link_libraries(netcoredbg
    corguids
    dbgshim
    mscordaccore
    palrt
)

install_clr(netcoredbg)
