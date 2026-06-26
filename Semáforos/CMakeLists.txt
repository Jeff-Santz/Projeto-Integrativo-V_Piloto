cmake_minimum_required(VERSION 3.10)
project(MasterControllerServer)

set(CMAKE_CXX_STANDARD 17)

# Cria o executável
add_executable(server main.cpp)

# Linka a biblioteca de sockets do Windows
if(WIN32)
    target_link_libraries(server ws2_32)
endif()