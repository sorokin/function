cmake_minimum_required(VERSION 3.15)

project(function)
include_directories(.)
add_subdirectory(gtest)

add_executable(function_testing
    function.h
    function_impl.h
    tests.cpp)

set_property(TARGET function_testing PROPERTY CXX_STANDARD 17)

target_link_libraries(function_testing gtest)
