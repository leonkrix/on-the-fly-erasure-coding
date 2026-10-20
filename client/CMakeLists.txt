cmake_minimum_required(VERSION 3.13.4)
project(client)

set(CMAKE_CXX_STANDARD 17)

add_executable(udpSender sender.cpp)
add_executable(udpReceiver receiver.cpp)

find_package(spdlog REQUIRED)

target_link_libraries(udpSender moeprlnc)
target_link_libraries(udpSender moepgf)
target_link_libraries(udpReceiver moeprlnc)
target_link_libraries(udpReceiver moepgf)
target_link_libraries(udpSender spdlog::spdlog)
target_link_libraries(udpReceiver spdlog::spdlog)

