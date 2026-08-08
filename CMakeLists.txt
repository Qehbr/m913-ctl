cmake_minimum_required(VERSION 3.15)
project(m913-ctl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Version, in order of preference:
#
#   1. -DAPP_VERSION=x.y.z passed by the caller. Distro packaging builds from a
#      release tarball, which has no .git, so git describe cannot work there —
#      the packager already knows the version and passes it in. Note this has
#      to be checked FIRST: the git branch below assigns APP_VERSION as a
#      normal variable, which would shadow the -D cache entry and silently
#      turn a packaged build into "dev".
#   2. The nearest git tag (v1.0.3 → 1.0.3), for builds from a checkout.
#   3. "dev", when there is neither.
if(NOT APP_VERSION)
    execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(REGEX REPLACE "^v" "" APP_VERSION "${GIT_TAG}")
endif()
if(NOT APP_VERSION)
    set(APP_VERSION "dev")
endif()
message(STATUS "m913-ctl version: ${APP_VERSION}")

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
