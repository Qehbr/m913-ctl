cmake_minimum_required(VERSION 3.15)
project(m913-ctl VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(m913-ctl
    src/main.cpp
    src/usb.cpp
    src/protocol.cpp
    src/data.cpp
    src/config.cpp
)

target_include_directories(m913-ctl PRIVATE
    src/
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(m913-ctl PRIVATE
    ${LIBUSB_LIBRARIES}
)

target_compile_options(m913-ctl PRIVATE
    -Wall -Wextra
    ${LIBUSB_CFLAGS_OTHER}
)

install(TARGETS m913-ctl DESTINATION bin)
install(FILES udev/99-m913.rules DESTINATION /etc/udev/rules.d)
