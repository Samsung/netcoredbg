# Copyright (C) 2020 Samsung Electronics Co., Ltd.
# See the LICENSE file in the project root for more information.

include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/windows
    ${PROJECT_SOURCE_DIR}/src/unix
    ${PROJECT_SOURCE_DIR}/third_party
)

# you may define new tests by using this function, arguments are following:
# deftest(executable-name source.cpp...)
function(deftest name)
    add_executable(${name} ${ARGN})
    if (NOT WIN32)
        target_link_libraries(${name} testsuite ${CMAKE_THREAD_LIBS_INIT})
    else()
        target_link_libraries(${name} wsock32 ws2_32 testsuite ${CMAKE_THREAD_LIBS_INIT})
    endif()
    add_test(${name} ${name})
endfunction()

# test suite compiled as library which should be linked with each one discrete unit test
add_library(testsuite STATIC Catch2.cpp)
add_definitions(-DDO_NOT_USE_WMAIN=1)

find_package (Threads)

# currently defined unit tests
deftest(string_view string_view_test.cpp)
deftest(span span_test.cpp)
deftest(escaped_string ../utils/escaped_string.cpp escaped_string_test.cpp)

deftest(iosystem
    iosystem_test.cpp
    ${PROJECT_SOURCE_DIR}/src/windows/iosystem_win32.cpp
    ${PROJECT_SOURCE_DIR}/src/unix/iosystem_unix.cpp
)

deftest(streams
    streams_test.cpp
    ${PROJECT_SOURCE_DIR}/src/utils/streams.cpp
    ${PROJECT_SOURCE_DIR}/src/windows/iosystem_win32.cpp
    ${PROJECT_SOURCE_DIR}/src/unix/iosystem_unix.cpp
)

deftest(ioredirect
    ioredirect_test.cpp
    ${PROJECT_SOURCE_DIR}/src/utils/ioredirect.cpp
    ${PROJECT_SOURCE_DIR}/src/utils/streams.cpp
    ${PROJECT_SOURCE_DIR}/src/windows/iosystem_win32.cpp
    ${PROJECT_SOURCE_DIR}/src/unix/iosystem_unix.cpp
    ${PROJECT_SOURCE_DIR}/src/utils/logger.cpp
)
