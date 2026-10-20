cmake_minimum_required(VERSION 3.13.4)
project(simulator)

set(CMAKE_CXX_STANDARD 17)

add_executable(simulator main.cpp)
target_link_libraries(simulator moeprlnc)
target_link_libraries(simulator moepgf)
