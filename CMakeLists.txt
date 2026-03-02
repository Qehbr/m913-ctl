cmake_minimum_required(VERSION 3.15)
project(m913-ctl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Derive version from the nearest git tag (e.g. v1.0.3 → 1.0.3).
# Falls back to "dev" when building outside a git repo or without tags.
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(REGEX REPLACE "^v" "" APP_VERSION "${GIT_TAG}")
if(NOT APP_VERSION)
    set(APP_VERSION "dev")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

add_executable(m913-ctl
    src/main.cpp
    src/usb.cpp
    src/protocol.cpp
    src/data.cpp
    src/config.cpp
)

target_compile_definitions(m913-ctl PRIVATE APP_VERSION="${APP_VERSION}")

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
install(FILES udev/99-m913.rules DESTINATION lib/udev/rules.d)
