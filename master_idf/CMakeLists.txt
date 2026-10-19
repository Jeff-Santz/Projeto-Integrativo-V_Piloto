cmake_minimum_required(VERSION 3.16)
project(master_can CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

find_package(Threads REQUIRED)

add_executable(master_can
    src/main.cpp
    src/can/can_bus.cpp
    src/core/monitor_semaforos.cpp
)

target_include_directories(master_can PRIVATE include)
target_link_libraries(master_can PRIVATE PkgConfig::LIBUSB Threads::Threads)

if (WIN32)
    # setupapi: dependencia do libusb no Windows (enumeracao de dispositivos)
    target_link_libraries(master_can PRIVATE setupapi)
endif()
