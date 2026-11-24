## SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
## SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.5)

# Host unit tests for timpani-n.
#
# Built from the timpani-n tree with -DBUILD_TESTS=ON, or on its own:
#   cmake -S timpani-n/tests -B build-tests && cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
# The stand-alone form needs only a C++ compiler and GoogleTest, so the
# scheduler rules shared with the BPF program can be tested without the BPF
# toolchain. On a host without the Linux UAPI headers, compat/ supplies
# <linux/types.h>.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(timpani-n-tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    enable_testing()
endif()

find_package(GTest REQUIRED)

include(CheckIncludeFileCXX)
check_include_file_cxx(linux/types.h TIMPANI_HAVE_LINUX_TYPES_H)

# job_fsm.h: L2 execution-interval state machine and CBS window rules.
add_executable(test_job_fsm test_job_fsm.cpp)
target_include_directories(test_job_fsm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/bpf
)
if(NOT TIMPANI_HAVE_LINUX_TYPES_H)
    target_include_directories(test_job_fsm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/compat
    )
endif()
target_compile_options(test_job_fsm PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_job_fsm
    GTest::gtest
    GTest::gtest_main
    pthread
)

add_test(NAME JobFsmTest COMMAND test_job_fsm)
set_tests_properties(JobFsmTest PROPERTIES
    TIMEOUT 30
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
