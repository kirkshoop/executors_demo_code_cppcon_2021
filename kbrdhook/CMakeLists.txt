# Copyright (c) Kirk Shoop.
#
# This source code is licensed under the license found in the
# LICENSE.txt file in the root directory of this source tree.

file(GLOB example-sources "*.cpp")
foreach(file-path ${example-sources})
    string( REPLACE ".cpp" "" file-path-without-ext ${file-path} )
    get_filename_component(file-name ${file-path-without-ext} NAME)
    add_executable( ${file-name} ${file-path})
    target_link_libraries(${file-name} PUBLIC unifex)
    add_test(NAME "example-${file-name}" COMMAND ${file-name})
endforeach()
