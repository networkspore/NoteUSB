# tests/CMakeLists.txt
# Unit tests for NoteUSB module

cmake_minimum_required(VERSION 3.16)
project(NoteUSBTests)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
find_package(Boost REQUIRED)

# Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG release-1.11.0
)
FetchContent_MakeAvailable(googletest)

# Path to NoteDaemon and NoteUSB source
set(NOTEDAEMON_DIR "/home/iospore/Dev/Netnotes/NoteDaemon")
set(NOTEUSB_DIR "/home/iospore/Dev/Netnotes/NoteUSB")

# Test sources
set(TEST_SOURCES
    test_main.cpp
    ${NOTEUSB_DIR}/src/device_handler.cpp
    ${NOTEUSB_DIR}/src/device_discovery.cpp
    module_interface_test.cpp
    handler_registry_test.cpp
    configuration_test.cpp
)

# Create test executable
add_executable(note_usb_tests ${TEST_SOURCES})

target_include_directories(note_usb_tests PRIVATE
    ${NOTEUSB_DIR}/include
    ${NOTEUSB_DIR}/src
    ${NOTEDAEMON_DIR}/include
    ${NOTEDAEMON_DIR}/include/module_framework
    ${LIBUSB_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(note_usb_tests PRIVATE
    gtest
    gtest_main
    note_usb
    ${LIBUSB_LIBRARIES}
    pthread
    dl
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::boost
)

# Add tests to CTest
include(CTest)
include(GoogleTest)
gtest_discover_tests(note_usb_tests)

message(STATUS "NoteUSB Tests configured")