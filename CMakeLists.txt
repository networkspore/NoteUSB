cmake_minimum_required(VERSION 3.16)
project(note_usb VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)

# Path to NoteDaemon source
# NoteDaemon is at /home/iospore/Dev/Netnotes/NoteDaemon
set(NOTEDAEMON_DIR "/home/iospore/Dev/Netnotes/NoteDaemon")

# Source files for the module
set(MODULE_SOURCES
    src/module.cpp
    src/device_handler.cpp
    src/note_usb_session.cpp
    src/hid_streaming_thread.cpp
)

# Build as a shared library
add_library(note_usb SHARED ${MODULE_SOURCES})

# Include directories
target_include_directories(note_usb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NOTEDAEMON_DIR}/include
    ${NOTEDAEMON_DIR}/include/module_framework
    ${LIBUSB_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(note_usb PRIVATE
    ${LIBUSB_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    pthread
    dl
)

# Set the output name
set_target_properties(note_usb PROPERTIES
    OUTPUT_NAME "note_usb"
    PREFIX ""
)

# Install to the module directory
install(TARGETS note_usb
    LIBRARY DESTINATION /etc/netnotes/modules/note_usb
    PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
)

# Install config.json
install(FILES config.json
    DESTINATION /etc/netnotes/modules/note_usb
    PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
)

# Add monitor subdirectory
add_subdirectory(monitor)

# Add tests subdirectory (optional, can be disabled)
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

# Print build info
message(STATUS "NoteUSB Version: ${PROJECT_VERSION}")
message(STATUS "NoteDaemon directory: ${NOTEDAEMON_DIR}")