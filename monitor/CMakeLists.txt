cmake_minimum_required(VERSION 3.16)

# Find dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# NoteUSB Device Monitor
# This monitor watches the NoteUSB daemon and reattaches kernel drivers on exit
add_executable(note_usb_monitor
    note_usb_monitor.cpp
    ${CMAKE_SOURCE_DIR}/../NoteDaemon/src/device_registry.cpp
)

target_include_directories(note_usb_monitor PRIVATE
    ${CMAKE_SOURCE_DIR}/../NoteDaemon/include
    ${LIBUSB_INCLUDE_DIRS}
)

target_link_libraries(note_usb_monitor PRIVATE
    ${LIBUSB_LIBRARIES}
)

# Install monitor binary
install(TARGETS note_usb_monitor
    RUNTIME DESTINATION /etc/netnotes/modules/note_usb
    PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ WORLD_READ
)