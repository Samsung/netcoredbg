# Copyright (C) 2020 Samsung Electronics Co., Ltd.
# See the LICENSE file in the project root for more information.

include_directories(
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/third_party
)

# you may define new tests by using this function, arguments are following:
# deftest(executable-name source.cpp...)
function(deftest name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} testsuite ${CMAKE_THREAD_LIBS_INIT})
    add_test(${name} ${name})
endfunction()

# test suite compiled as library which should be linked with each one discrete unit test
add_library(testsuite STATIC Catch2.cpp)

find_package (Threads)

# currently defined unit tests
deftest(string_view string_view_test.cpp)
deftest(span span_test.cpp)
deftest(escaped_string ../utils/escaped_string.cpp escaped_string_test.cpp)
