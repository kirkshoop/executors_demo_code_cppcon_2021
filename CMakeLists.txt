# Copyright (c) Kirk Shoop.
#
# This source code is licensed under the license found in the
# LICENSE.txt file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.12)

project(executors_demos LANGUAGES CXX
                  VERSION 0.1)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libunifex/cmake")

# We enable testing in both cases, either if doing tests or building examples
# Note, that examples are also registered as tests by subsequent calls to add_test()
# You can run all tests or examples at once by using `ctest` or `ninja test`
include(CTest)
add_subdirectory(libunifex)
add_subdirectory(kbrdhook)
