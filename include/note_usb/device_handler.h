// include/note_usb/device_handler.h
// Device handler implementation for NoteUSB module
// Handles USB device discovery, claiming, and release

#ifndef NOTE_USB_DEVICE_HANDLER_H
#define NOTE_USB_DEVICE_HANDLER_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <libusb-1.0/libusb.h>
#include <boost/multiprecision/cpp_int.hpp>

// Include NoteDaemon headers for type definitions
#include "module_framework/error.h"
#include "notebytes.h"
#include "note_messaging.h"

// Include module-private session manager
#include "session_manager.h"

namespace NoteUSB {

// Using aliases for clarity
using cpp_int = boost::multiprecision::cpp_int;

/**
 * Device information for discovered devices
 */
struct USBDeviceDescriptor {
    std::string device_id;        // bus:address format (e.g. "1:2")
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    int interface_number = 0;
    bool kernel_driver_attached = false;
    libusb_device_handle* handle = nullptr;  // Set when claimed
    pid_t owner_pid = 0;  // Process ID of the client that claimed this device

    // Convert to NoteBytes::Object for serialization
    NoteBytes::Object to_notebytes() const;
};

/**
 * Claimed device tracked in the registry
 */
struct ClaimedDevice {
    pid_t pid = 0;
    std::string device_id;
    int interface_number = 0;
    bool kernel_driver_attached = false;
};

/**
 * Device registry for crash recovery
 * File location: /run/netnotes/modules/note_usb/device_registry.json
 */
class DeviceRegistry {
public:
    static const std::string& path();

    /**
     * Add a claimed device to the registry.
     * Thread-safe — uses a mutex to protect concurrent writes.
     */
    static void add_device(const std::string& device_id, int interface_number, bool kernel_driver_attached);

    /**
     * Remove a claimed device from the registry (e.g. on clean release).
     * Returns true if the device was found and removed.
     */
    static bool remove_device(const std::string& device_id);

    /**
     * Get all devices in the registry.
     * Used by the monitor process to clean up everything.
     */
    static std::vector<ClaimedDevice> get_all_devices();

    /**
     * Remove all devices.
     * Called by the monitor after reattaching.
     */
    static void remove_all();

private:
    static std::mutex& registry_mutex();
    static std::string& registry_path();

    /**
     * Read the registry file and parse into a vector of ClaimedDevice.
     * Returns empty vector on error.
     */
    static std::vector<ClaimedDevice> read_registry();

    /**
     * Write the registry vector back to the file.
     * Returns false on error.
     */
    static bool write_registry(const std::vector<ClaimedDevice>& devices);

    /**
     * Simple JSON string builder — no external dependencies.
     */
    static std::string json_object(const ClaimedDevice& device);
};

class DeviceHandler {
public:
    DeviceHandler();
    ~DeviceHandler();

    // Discovery
    void start_discovery();
    void stop_discovery();
    void discover_devices();

    // Device operations
    NoteDaemon::Error claim_device(const NoteBytes::Object& msg);
    NoteDaemon::Error release_device(const NoteBytes::Object& msg);
    void send_device_list();

    // Cleanup
    void release_all_devices();
    void collect_errors(std::vector<NoteDaemon::Error>& errors);

    // ===== Device State =====

    struct DeviceInfo {
        std::string device_id;
        libusb_device_handle* handle = nullptr;
        int interface_number = 0;
        bool kernel_driver_attached = false;
        cpp_int capabilities;  // Device capabilities
    };

    std::map<std::string, DeviceInfo> claimed_devices_;
    std::map<std::string, std::shared_ptr<USBDeviceDescriptor>> available_devices_;

    // Getters for device state - must come after DeviceInfo struct
    const std::map<std::string, std::shared_ptr<USBDeviceDescriptor>>& available_devices() const { return available_devices_; }
    const std::map<std::string, DeviceInfo>& claimed_devices() const { return claimed_devices_; }

    // Helper methods
    bool is_hid_device(libusb_device* device);
    std::shared_ptr<USBDeviceDescriptor> find_device_by_id(const std::string& device_id);
    NoteDaemon::Error send_response(const NoteBytes::Object& response);

private:
    libusb_context* ctx_ = nullptr;
    bool running_ = false;
    int discovery_interval_ms_ = 1000;
};

} // namespace NoteUSB

#endif // NOTE_USB_DEVICE_HANDLER_H
