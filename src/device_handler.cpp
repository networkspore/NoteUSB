// src/device_handler.cpp
// Device handler implementation for NoteUSB module

#include "note_usb/device_handler.h"
#include "note_usb/session_manager.h"
#include <syslog.h>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <fstream>

// Include NoteDaemon headers for NoteBytes and messaging
#include "note_messaging.h"
#include "notebytes.h"
#include "event_bytes.h"
#include "capability_registry.h"

namespace NoteUSB {

// =============================================================================
// USBDeviceDescriptor implementation
// =============================================================================

NoteBytes::Object USBDeviceDescriptor::to_notebytes() const {
    NoteBytes::Object obj;
    obj.add(NoteBytes::Value("device_id"), NoteBytes::Value(device_id));
    obj.add(NoteBytes::Value("vendor_id"), NoteBytes::Value((int)vendor_id));
    obj.add(NoteBytes::Value("product_id"), NoteBytes::Value((int)product_id));
    obj.add(NoteBytes::Value("interface_number"), NoteBytes::Value(interface_number));
    return obj;
}

// =============================================================================
// DeviceRegistry implementation (module-private)
// =============================================================================

const std::string& DeviceRegistry::path() {
    static std::string path = "/run/netnotes/modules/note_usb/device_registry.json";
    return path;
}

void DeviceRegistry::add_device(const std::string& device_id, int interface_number, bool kernel_driver_attached) {
    std::lock_guard<std::mutex> lock(registry_mutex());

    ClaimedDevice device;
    device.pid = getpid();  // Daemon's PID
    device.device_id = device_id;
    device.interface_number = interface_number;
    device.kernel_driver_attached = kernel_driver_attached;

    auto devices = read_registry();

    // Remove any existing entry for this device (idempotent)
    devices.erase(
        std::remove_if(devices.begin(), devices.end(),
            [&device](const ClaimedDevice& d) {
                return d.device_id == device.device_id;
            }),
        devices.end());

    devices.push_back(device);
    write_registry(devices);

    syslog(LOG_INFO, "DeviceRegistry: added device %s (interface %d)", device_id.c_str(), interface_number);
}

bool DeviceRegistry::remove_device(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(registry_mutex());

    auto devices = read_registry();
    auto it = std::remove_if(devices.begin(), devices.end(),
        [&device_id](const ClaimedDevice& d) {
            return d.device_id == device_id;
        });

    if (it == devices.end()) {
        syslog(LOG_WARNING, "DeviceRegistry: device %s not found in registry", device_id.c_str());
        return false;
    }

    devices.erase(it, devices.end());
    write_registry(devices);

    syslog(LOG_INFO, "DeviceRegistry: removed device %s", device_id.c_str());
    return true;
}

std::vector<ClaimedDevice> DeviceRegistry::get_all_devices() {
    std::lock_guard<std::mutex> lock(registry_mutex());
    return read_registry();
}

void DeviceRegistry::remove_all() {
    std::lock_guard<std::mutex> lock(registry_mutex());

    auto devices = read_registry();
    devices.clear();
    write_registry(devices);

    syslog(LOG_INFO, "DeviceRegistry: removed all devices");
}

// =============================================================================

std::mutex& DeviceRegistry::registry_mutex() {
    static std::mutex mtx;
    return mtx;
}

std::string& DeviceRegistry::registry_path() {
    static std::string path = "/run/netnotes/modules/note_usb/device_registry.json";
    return path;
}

std::vector<ClaimedDevice> DeviceRegistry::read_registry() {
    std::vector<ClaimedDevice> devices;

    struct stat st;
    if (stat(registry_path().c_str(), &st) != 0) {
        // File doesn't exist yet — nothing to read
        return devices;
    }

    std::ifstream file(registry_path());
    if (!file.is_open()) {
        syslog(LOG_WARNING, "DeviceRegistry: failed to open registry: %s",
               registry_path().c_str());
        return devices;
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Simple JSON array parser — no external dependencies
    // Format: [{"pid":12345,"device_id":"1:3","interface_number":0,"kernel_driver_attached":true},...]

    // Find all object boundaries
    size_t pos = 0;
    while (pos < content.size()) {
        // Find the start of an object
        size_t obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find the matching end
        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < content.size(); i++) {
            if (content[i] == '{') depth++;
            if (content[i] == '}') {
                depth--;
                if (depth == 0) {
                    obj_end = i;
                    break;
                }
            }
        }

        if (obj_end == obj_start) {
            pos = obj_start + 1;
            continue;
        }

        // Parse this object
        std::string obj = content.substr(obj_start + 1, obj_end - obj_start - 1);

        ClaimedDevice device;
        device.pid = 0;
        device.interface_number = 0;
        device.kernel_driver_attached = false;

        // Parse key-value pairs
        size_t key_pos = 0;
        while (key_pos < obj.size()) {
            // Find key
            size_t key_start = obj.find('"', key_pos);
            if (key_start == std::string::npos) break;

            size_t key_end = obj.find('"', key_start + 1);
            if (key_end == std::string::npos) break;

            std::string key = obj.substr(key_start + 1, key_end - key_start - 1);

            // Skip to value
            size_t val_start = obj.find(':', key_end + 1);
            if (val_start == std::string::npos) break;

            // Parse value based on key
            if (key == "pid") {
                size_t val_end = obj.find(',', val_start + 1);
                if (val_end == std::string::npos) val_end = obj.find('}', val_start + 1);
                std::string val = obj.substr(val_start + 1, val_end - val_start - 1);
                try {
                    device.pid = std::stoll(val);
                } catch (...) {
                    device.pid = 0;
                }
            } else if (key == "device_id") {
                size_t val_start2 = obj.find('"', val_start + 1);
                size_t val_end2 = obj.find('"', val_start2 + 1);
                if (val_start2 != std::string::npos && val_end2 != std::string::npos) {
                    device.device_id = obj.substr(val_start2 + 1, val_end2 - val_start2 - 1);
                }
            } else if (key == "interface_number") {
                size_t val_end = obj.find(',', val_start + 1);
                if (val_end == std::string::npos) val_end = obj.find('}', val_start + 1);
                std::string val = obj.substr(val_start + 1, val_end - val_start - 1);
                try {
                    device.interface_number = std::stoi(val);
                } catch (...) {
                    device.interface_number = 0;
                }
            } else if (key == "kernel_driver_attached") {
                std::string val = obj.substr(val_start + 1);
                // Trim whitespace and closing brace
                size_t end = val.find('}');
                if (end != std::string::npos) val = val.substr(0, end);
                size_t end2 = val.find(',');
                if (end2 != std::string::npos) val = val.substr(0, end2);
                // Trim
                size_t start = val.find_first_not_of(" \t\r\n");
                if (start != std::string::npos) {
                    val = val.substr(start);
                }
                device.kernel_driver_attached = (val == "true");
            }

            // Move past this key-value pair
            key_pos = (key_end + 1 < obj.size()) ? obj.find('"', key_end + 1) : std::string::npos;
        }

        // Only add if we got a valid device
        if (!device.device_id.empty()) {
            devices.push_back(device);
        }

        pos = obj_end + 1;
    }

    return devices;
}

bool DeviceRegistry::write_registry(const std::vector<ClaimedDevice>& devices) {
    // Ensure directory exists
    std::string dir = registry_path();
    size_t last_slash = dir.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir_path = dir.substr(0, last_slash);
        mkdir(dir_path.c_str(), 0755);
    }

    std::ofstream file(registry_path());
    if (!file.is_open()) {
        syslog(LOG_WARNING, "DeviceRegistry: failed to write registry: %s",
               registry_path().c_str());
        return false;
    }

    file << "[";
    for (size_t i = 0; i < devices.size(); i++) {
        if (i > 0) file << ", ";
        file << json_object(devices[i]);
    }
    file << "]" << std::endl;

    return true;
}

std::string DeviceRegistry::json_object(const ClaimedDevice& device) {
    std::ostringstream oss;
    oss << "{"
        << "\"pid\":" << device.pid << ","
        << "\"device_id\":\"" << device.device_id << "\","
        << "\"interface_number\":" << device.interface_number << ","
        << "\"kernel_driver_attached\":" << (device.kernel_driver_attached ? "true" : "false")
        << "}";
    return oss.str();
}

// =============================================================================
// DeviceHandler implementation
// =============================================================================

DeviceHandler::DeviceHandler() {
    // Initialize libusb
    int rc = libusb_init(&ctx_);
    if (rc != 0) {
        syslog(LOG_ERR, "NoteUSB: failed to init libusb: %s", libusb_error_name(rc));
    }
}

DeviceHandler::~DeviceHandler() {
    release_all_devices();
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

void DeviceHandler::start_discovery() {
    syslog(LOG_INFO, "[DeviceHandler] start_discovery() called");
    if (running_) {
        syslog(LOG_WARNING, "[DeviceHandler] start_discovery() called but already running");
        return;
    }

    running_ = true;
    syslog(LOG_INFO, "[DeviceHandler] start_discovery() - setting running=true");
    syslog(LOG_INFO, "NoteUSB: device discovery started");

    // Perform initial discovery
    syslog(LOG_INFO, "[DeviceHandler] start_discovery() - calling discover_devices()");
    discover_devices();
    syslog(LOG_INFO, "[DeviceHandler] start_discovery() - discover_devices() completed");
}

void DeviceHandler::stop_discovery() {
    running_ = false;
    syslog(LOG_INFO, "NoteUSB: device discovery stopped");
}

void DeviceHandler::discover_devices() {
    syslog(LOG_INFO, "[DeviceHandler] discover_devices() called");
    if (!ctx_) {
        syslog(LOG_ERR, "[DeviceHandler] discover_devices() - libusb not initialized");
        return;
    }

    syslog(LOG_INFO, "[DeviceHandler] discover_devices() - about to call libusb_get_device_list");
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &device_list);
    syslog(LOG_INFO, "[DeviceHandler] discover_devices() - libusb_get_device_list returned %zd", count);

    if (count < 0) {
        syslog(LOG_ERR, "[DeviceHandler] discover_devices() - failed to get USB device list: %s",
               libusb_error_name((int)count));
        return;
    }

    syslog(LOG_INFO, "NoteUSB: scanning %zd USB devices", count);

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = device_list[i];

        // Check if this is a HID device
        if (!is_hid_device(device)) {
            continue;
        }

        // Get device descriptor
        struct libusb_device_descriptor desc;
        int result = libusb_get_device_descriptor(device, &desc);
        if (result != LIBUSB_SUCCESS) {
            syslog(LOG_WARNING, "NoteUSB: failed to get device descriptor: %s",
                   libusb_error_name(result));
            continue;
        }

        // Create device ID from bus:address
        uint8_t bus = libusb_get_bus_number(device);
        uint8_t address = libusb_get_device_address(device);
        std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

        // Just collect device info - don't try to open it!
        // Opening devices will happen in claim_device() when actually needed
        auto device_desc = std::make_shared<USBDeviceDescriptor>();
        device_desc->device_id = device_id;
        device_desc->vendor_id = desc.idVendor;
        device_desc->product_id = desc.idProduct;
        device_desc->interface_number = 0;  // Usually 0 for HID
        device_desc->kernel_driver_attached = false;
        device_desc->handle = nullptr;  // Will be opened when claimed

        available_devices_[device_id] = device_desc;
        syslog(LOG_INFO, "NoteUSB: discovered HID device: %s (VID:PID %04x:%04x)",
               device_id.c_str(), desc.idVendor, desc.idProduct);
    }

    libusb_free_device_list(device_list, 1);
    syslog(LOG_INFO, "NoteUSB: device discovery complete. Found %zu HID devices",
           available_devices_.size());
}

NoteDaemon::Error DeviceHandler::claim_device(const NoteBytes::Object& msg) {
    // Extract device_id from message
    auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
    if (!device_id_val) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                                            "Missing DEVICE_ID field");
    }
    std::string device_id = device_id_val->as_string();

    syslog(LOG_INFO, "NoteUSB: claim device request for %s", device_id.c_str());

    // Check if device exists in available devices
    auto device_it = available_devices_.find(device_id);
    if (device_it == available_devices_.end()) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
                                            "Device not found: " + device_id);
    }

    // Check if already claimed
    if (claimed_devices_.find(device_id) != claimed_devices_.end()) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::ITEM_NOT_AVAILABLE,
                                            "Device already claimed: " + device_id);
    }

    // Find the actual libusb_device
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &device_list);
    libusb_device* usb_device = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
        uint8_t bus = libusb_get_bus_number(device_list[i]);
        uint8_t address = libusb_get_device_address(device_list[i]);
        std::string current_id = std::to_string(bus) + ":" + std::to_string(address);
        if (current_id == device_id) {
            usb_device = device_list[i];
            break;
        }
    }

    if (!usb_device) {
        libusb_free_device_list(device_list, 1);
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
                                            "USB device not found: " + device_id);
    }

    // Open device handle
    libusb_device_handle* handle = nullptr;
    int result = libusb_open(usb_device, &handle);
    libusb_free_device_list(device_list, 1);

    if (result != LIBUSB_SUCCESS) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::PERMISSION_DENIED,
                                            "Cannot open device " + device_id + ": " +
                                            libusb_error_name(result));
    }

    auto device_desc = device_it->second;
    int interface_number = device_desc->interface_number;

    // Claim interface
    result = libusb_claim_interface(handle, interface_number);
    if (result != LIBUSB_SUCCESS) {
        libusb_close(handle);
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::PERMISSION_DENIED,
                                            "Cannot claim interface for device " + device_id + ": " +
                                            libusb_error_name(result));
    }

    // Detach kernel driver if active
    bool kernel_driver_attached = false;
    if (libusb_kernel_driver_active(handle, interface_number) == 1) {
        result = libusb_detach_kernel_driver(handle, interface_number);
        if (result == LIBUSB_SUCCESS) {
            kernel_driver_attached = true;
            syslog(LOG_INFO, "NoteUSB: detached kernel driver for device %s", device_id.c_str());
        } else {
            syslog(LOG_WARNING, "NoteUSB: failed to detach kernel driver for device %s: %s",
                   device_id.c_str(), libusb_error_name(result));
        }
    }

    // Store device info
    DeviceInfo info;
    info.device_id = device_id;
    info.handle = handle;
    info.interface_number = interface_number;
    info.kernel_driver_attached = kernel_driver_attached;
    info.capabilities = Capabilities::Masks::mode_mask();  // Default capabilities

    claimed_devices_[device_id] = info;

    // Update device descriptor
    device_desc->handle = handle;
    device_desc->kernel_driver_attached = kernel_driver_attached;

    // Register with module-private DeviceRegistry
    DeviceRegistry::add_device(device_id, interface_number, kernel_driver_attached);

    syslog(LOG_INFO, "NoteUSB: successfully claimed device: %s", device_id.c_str());

    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

NoteDaemon::Error DeviceHandler::release_device(const NoteBytes::Object& msg) {
    // Extract device_id from message
    auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
    if (!device_id_val) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                                            "Missing DEVICE_ID field");
    }
    std::string device_id = device_id_val->as_string();

    syslog(LOG_INFO, "NoteUSB: release device request for %s", device_id.c_str());

    // Check if device is claimed
    auto state_it = claimed_devices_.find(device_id);
    if (state_it == claimed_devices_.end()) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
                                            "Device not claimed: " + device_id);
    }

    auto& info = state_it->second;

    // Release interface
    if (info.handle) {
        libusb_release_interface(info.handle, info.interface_number);

        // Reattach kernel driver if it was detached
        if (info.kernel_driver_attached) {
            int result = libusb_attach_kernel_driver(info.handle, info.interface_number);
            if (result == LIBUSB_SUCCESS) {
                syslog(LOG_INFO, "NoteUSB: reattached kernel driver for device %s", device_id.c_str());
            } else {
                syslog(LOG_WARNING, "NoteUSB: failed to reattach kernel driver for device %s: %s",
                       device_id.c_str(), libusb_error_name(result));
            }
        }

        // Close handle
        libusb_close(info.handle);
    }

    // Remove from claimed devices
    claimed_devices_.erase(state_it);

    // Update device descriptor in available devices
    auto device_it = available_devices_.find(device_id);
    if (device_it != available_devices_.end()) {
        device_it->second->handle = nullptr;
        device_it->second->kernel_driver_attached = false;
    }

    // Unregister from module-private DeviceRegistry
    DeviceRegistry::remove_device(device_id);

    syslog(LOG_INFO, "NoteUSB: successfully released device: %s", device_id.c_str());

    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

void DeviceHandler::send_device_list() {
    // This would need to be implemented with actual response mechanism
    // For now, just log the device list
    syslog(LOG_INFO, "NoteUSB: sending device list with %zu devices", available_devices_.size());

    for (const auto& [id, device] : available_devices_) {
        syslog(LOG_DEBUG, "NoteUSB:   - %s (VID:PID %04x:%04x)",
               id.c_str(), device->vendor_id, device->product_id);
    }
}

void DeviceHandler::release_all_devices() {
    for (auto& [device_id, info] : claimed_devices_) {
        if (info.handle) {
            // Release interface
            libusb_release_interface(info.handle, info.interface_number);

            // Reattach kernel driver if detached
            if (info.kernel_driver_attached) {
                libusb_attach_kernel_driver(info.handle, info.interface_number);
            }

            // Close handle
            libusb_close(info.handle);
        }
    }
    claimed_devices_.clear();

    // Clear available devices
    available_devices_.clear();

    // Remove all from registry
    DeviceRegistry::remove_all();

    syslog(LOG_INFO, "NoteUSB: released all devices");
}

void DeviceHandler::collect_errors(std::vector<NoteDaemon::Error>& errors) {
    // No errors to collect for now
}

// =============================================================================
// Helper methods
// =============================================================================

bool DeviceHandler::is_hid_device(libusb_device* device) {
    libusb_config_descriptor* config = nullptr;

    if (libusb_get_active_config_descriptor(device, &config) != LIBUSB_SUCCESS) {
        return false;
    }

    bool is_hid = false;
    for (int j = 0; j < config->bNumInterfaces; ++j) {
        const struct libusb_interface* interface = &config->interface[j];
        if (interface->num_altsetting > 0) {
            const struct libusb_interface_descriptor* altsetting = &interface->altsetting[0];
            if (altsetting->bInterfaceClass == LIBUSB_CLASS_HID) {
                is_hid = true;
                break;
            }
        }
    }

    libusb_free_config_descriptor(config);
    return is_hid;
}

std::shared_ptr<USBDeviceDescriptor> DeviceHandler::find_device_by_id(const std::string& device_id) {
    auto it = available_devices_.find(device_id);
    if (it != available_devices_.end()) {
        return it->second;
    }
    return nullptr;
}

NoteDaemon::Error DeviceHandler::send_response(const NoteBytes::Object& response) {
    // This is a placeholder - in the full implementation, this would send
    // the response back to the client through the session manager
    syslog(LOG_DEBUG, "NoteUSB: send_response called");
    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

} // namespace NoteUSB
