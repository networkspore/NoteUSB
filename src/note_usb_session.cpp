// src/note_usb_session.cpp
// NoteUSB session implementation - one device per connection

#include "note_usb/note_usb_session.h"
#include "note_usb/session_manager.h"
#include "module_framework/encryption_api.h"
#include "module_framework/error.h"
#include "notebytes_writer.h"
#include "notebytes_reader.h"
#include <syslog.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <poll.h>

using namespace NoteUSB;

// =============================================================================
// NoteUSBSession implementation
// =============================================================================

NoteUSBSession::NoteUSBSession(libusb_context* usb_ctx, int client_fd, pid_t client_pid,
                               const std::string& device_id)
    : usb_ctx_(usb_ctx), client_fd_(client_fd), client_pid_(client_pid), device_id_(device_id) {

    syslog(LOG_INFO, "NoteUSBSession: created for client pid=%d, device_id=%s",
           client_pid, device_id.empty() ? "(discovery)" : device_id.c_str());
    
    // Register for hotplug notifications
    register_for_hotplug();
}

NoteUSBSession::~NoteUSBSession() {
    // Unregister from hotplug notifications
    unregister_from_hotplug();
    
    // Stop streaming thread
    stop_streaming();
    
    // Note: We don't call stop() here because we want to keep the session
    // alive for potential device re-claim. The session will be destroyed
    // only when the client disconnects.
    
    syslog(LOG_INFO, "NoteUSBSession: destroyed for client pid=%d", client_pid_);
}

void NoteUSBSession::start() {
    if (running_.load(std::memory_order_relaxed)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    // If device_id was provided, try to claim it
    if (!device_id_.empty()) {
        auto err = claim_device(device_id_);
        if (err.failed()) {
            syslog(LOG_ERR, "NoteUSBSession: failed to claim device %s: %s",
                   device_id_.c_str(), err.message().data());
            running_.store(false, std::memory_order_release);
            return;
        }
        
        // Start streaming thread
        start_streaming();
    }

    // Start reading from socket
    read_socket();

    syslog(LOG_INFO, "NoteUSBSession: started for client pid=%d", client_pid_);
}

void NoteUSBSession::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);

    // Stop streaming thread (if running)
    stop_streaming();

    // Note: We don't release the device here. The session stays open
    // so the client can claim a new device later. Routing remains locked in.
    // The device will be released when the client disconnects or explicitly
    // requests a device release.
    
    syslog(LOG_INFO, "NoteUSBSession: stopped for client pid=%d", client_pid_);
}

bool NoteUSBSession::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

void NoteUSBSession::handle_message(const NoteBytes::Object& message) {
    auto* event_val = message.get(NoteMessaging::Keys::EVENT);

    // Protocol standard: message type is always carried in EVENT.
    NoteBytes::Value event_type = event_val ? *event_val : NoteBytes::Value("");

    // Check if event is present (Value with string type and empty data means empty)
    bool has_event = event_val && !(event_type.type() == NoteBytes::Type::STRING && event_type.data().empty());

    // Log detailed info about what was received
    syslog(LOG_INFO, "NoteUSBSession: received message with %zu fields, event_val=%p, event_type=%s, has_event=%d",
           message.size(), (void*)event_val, event_type.as_string().c_str(), has_event);
    
    // Log all fields in the message for debugging
    for (const auto& pair : message.pairs()) {
        const auto& key = pair.key();
        const auto& value = pair.value();
        syslog(LOG_DEBUG, "NoteUSBSession:   field: key_type=%d, key_data_size=%zu, value_type=%d, value_data_size=%zu",
               key.type(), key.size(), value.type(), value.size());
    }

    if (has_event) {
        // Route by event type - compare Value directly
        if (event_type == NoteMessaging::ProtocolMessages::HELLO) {
            NoteBytes::Object response;
            response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ACCEPT);
            response.add(NoteMessaging::Keys::STATUS, "accepted");
            send_response(response);
        } else if (event_type == NoteMessaging::ProtocolMessages::REQUEST_DISCOVERY) {
            handle_discover(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::CLAIM_ITEM) {
            handle_claim(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::RELEASE_ITEM) {
            handle_release(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::RESUME) {
            handle_resume(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION) {
            handle_enable_encryption(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::DISABLE_ENCRYPTION) {
            handle_disable_encryption(message);
        } else if (event_type == NoteMessaging::ProtocolMessages::SHUTDOWN) {
            // Handle graceful shutdown request from client
            syslog(LOG_INFO, "NoteUSBSession: received shutdown request from client (pid=%d)", client_pid_);
            
            // Send acknowledgment
            NoteBytes::Object response;
            response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::SHUTDOWN);
            response.add(NoteMessaging::Keys::STATUS, "ok");
            send_response(response);
            
            // Set stop flag to end the session
            stop_requested_.store(true, std::memory_order_release);
        } else {
            send_error(event_type, "", NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                      std::string("Unknown event type: ") + event_type.as_string());
        }
    } else {
        send_error(NoteMessaging::ProtocolMessages::ERROR, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Message missing required EVENT field");
    }
}

void NoteUSBSession::read_socket() {
    syslog(LOG_INFO, "NoteUSBSession: read_socket started (pid=%d)", client_pid_);
    
    for (;;) {
        if (stop_requested_.load(std::memory_order_relaxed)) {
            syslog(LOG_INFO, "NoteUSBSession: stop requested, exiting read loop (pid=%d)", client_pid_);
            break;
        }

        try {
            // Read message from socket as a NoteBytes Object
            NoteBytes::Reader reader(client_fd_);
            syslog(LOG_DEBUG, "NoteUSBSession: waiting for message (pid=%d)", client_pid_);
            
            auto event_obj = reader.read_object();
            
            syslog(LOG_DEBUG, "NoteUSBSession: successfully parsed message object (pid=%d)", client_pid_);

            // Handle the message
            handle_message(event_obj);
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "NoteUSBSession: error reading message (pid=%d): %s", client_pid_, e.what());
            // Don't crash - log the error and continue
            // The session will be cleaned up when the client disconnects
            break;
        }

        if (stop_requested_.load(std::memory_order_relaxed)) {
            break;
        }
    }

    syslog(LOG_INFO, "NoteUSBSession: read_socket ended for client pid=%d", client_pid_);
}

// =============================================================================
// Message Handlers
// =============================================================================

void NoteUSBSession::handle_discover(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_discover");

    // Discover devices
    auto err = discover_devices();
    if (err.failed()) {
        send_error(NoteMessaging::ProtocolMessages::ITEM_LIST, "",
                  err.code, std::string(err.message()));
        return;
    }

    // Send device list
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_LIST);

    NoteBytes::Array devices_array;
    if (device_) {
        // If we have a device, send just that one
        auto device_obj = device_->to_notebytes();
        devices_array.add(NoteBytes::Value(device_obj.serialize(), NoteBytes::Type::OBJECT));
    } else {
        // Send all available devices
        // Note: This would need to iterate through available_devices_
        // For now, just send empty list
    }

    response.add(NoteMessaging::Keys::ITEMS, devices_array.as_value());

    send_response(response);
    syslog(LOG_INFO, "NoteUSBSession: sent device list for client pid=%d", client_pid_);
}

void NoteUSBSession::handle_claim(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_claim");

    auto* device_id_val = message.get(NoteMessaging::Keys::DEVICE_ID);

    std::string device_id = device_id_val ? device_id_val->as_string() : "";

    if (device_id.empty()) {
        send_error(NoteMessaging::ProtocolMessages::ITEM_CLAIMED, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing DEVICE_ID field");
        return;
    }

    // Claim the device
    auto err = claim_device(device_id);
    if (err.failed()) {
        send_error(NoteMessaging::ProtocolMessages::ITEM_CLAIMED, device_id,
                  err.code, std::string(err.message()));
        return;
    }

    // Send success response
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_CLAIMED);
    response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    response.add(NoteMessaging::Keys::STATUS, "claimed");

    send_response(response);
    syslog(LOG_INFO, "NoteUSBSession: device claimed: %s for client pid=%d",
           device_id.c_str(), client_pid_);
}

void NoteUSBSession::handle_release(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_release");

    auto* device_id_val = message.get(NoteMessaging::Keys::DEVICE_ID);

    std::string device_id = device_id_val ? device_id_val->as_string() : "";

    if (device_id.empty()) {
        send_error(NoteMessaging::ProtocolMessages::ITEM_RELEASED, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing DEVICE_ID field");
        return;
    }

    // Release the device
    auto err = release_device();
    if (err.failed()) {
        send_error(NoteMessaging::ProtocolMessages::ITEM_RELEASED, device_id,
                  err.code, std::string(err.message()));
        return;
    }

    // Send success response
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_RELEASED);
    response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    response.add(NoteMessaging::Keys::STATUS, NoteMessaging::ProtocolMessages::SUCCESS);

    send_response(response);
    syslog(LOG_INFO, "NoteUSBSession: device released for client pid=%d", client_pid_);
}

void NoteUSBSession::handle_resume(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_resume");

    int processed_count = message.get_int(std::string_view("processed_count"), 0);
    std::string device_id = message.get_string(NoteMessaging::Keys::DEVICE_ID,
                                               NoteMessaging::Keys::EMPTY);

    if (device_id.empty()) {
        send_error(NoteMessaging::ProtocolMessages::RESUME, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing DEVICE_ID field");
        return;
    }

    syslog(LOG_DEBUG, "NoteUSBSession: resume acknowledged %d messages for device %s",
           processed_count, device_id.c_str());

    // TODO: Implement resume logic
    // For now, just acknowledge
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::RESUME);
    response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    response.add(NoteMessaging::Keys::STATUS, "ok");

    send_response(response);
}

// =============================================================================
// Encryption Handlers
// =============================================================================

void NoteUSBSession::handle_enable_encryption(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_enable_encryption");

    auto* device_id_val = message.get(NoteMessaging::Keys::DEVICE_ID);
    auto* key_val = message.get(NoteMessaging::ProtocolMessages::KEY);

    std::string device_id = device_id_val ? device_id_val->as_string() : "";

    if (device_id.empty()) {
        send_error(NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing DEVICE_ID field");
        return;
    }

    if (!key_val) {
        send_error(NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION, device_id,
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing KEY field");
        return;
    }

    // Extract key bytes from NoteBytes::Value
    std::vector<uint8_t> key;
    if (key_val->type() == NoteBytes::Type::ARRAY || key_val->type() == NoteBytes::Type::INTEGER_ARRAY) {
        // Manually read values from the array data
        const auto& data = key_val->data();
        size_t offset = 0;
        while (offset < data.size()) {
            // Peek at the next value's metadata
            if (offset + NoteBytes::METADATA_SIZE > data.size()) {
                break;  // Not enough data
            }
            NoteBytes::Value elem = NoteBytes::Value::read_from(data.data(), offset);
            key.push_back(elem.as_byte());
        }
    } else {
        send_error(NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION, device_id,
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "KEY must be an array of bytes");
        return;
    }

    // Initialize encryption for this device
    auto& encryption_provider = NoteDaemon::get_encryption_provider();
    NoteDaemon::Error err = encryption_provider.init_device(device_id, key);

    if (err.failed()) {
        send_error(NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION, device_id,
                  err.code, std::string(err.message()));
        return;
    }

    // Store encryption key for this session
    encryption_key_ = key;
    encryption_enabled_ = true;

    // Send success response
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ENCRYPTION_READY);
    response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    response.add(NoteMessaging::Keys::STATUS, NoteMessaging::ProtocolMessages::SUCCESS);

    send_response(response);
    syslog(LOG_INFO, "NoteUSBSession: encryption enabled for device %s", device_id.c_str());
}

void NoteUSBSession::handle_disable_encryption(const NoteBytes::Object& message) {
    syslog(LOG_DEBUG, "NoteUSBSession: handle_disable_encryption");

    auto* device_id_val = message.get(NoteMessaging::Keys::DEVICE_ID);

    std::string device_id = device_id_val ? device_id_val->as_string() : "";

    if (device_id.empty()) {
        send_error(NoteMessaging::ProtocolMessages::DISABLE_ENCRYPTION, "",
                  NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                  "Missing DEVICE_ID field");
        return;
    }

    // Remove encryption context
    auto& encryption_provider = NoteDaemon::get_encryption_provider();
    encryption_provider.remove_device(device_id);

    // Clear session encryption
    encryption_key_.clear();
    encryption_enabled_ = false;

    // Send success response
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ENABLE_ENCRYPTION);
    response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    response.add(NoteMessaging::Keys::STATUS, NoteMessaging::ProtocolMessages::SUCCESS);

    send_response(response);
    syslog(LOG_INFO, "NoteUSBSession: encryption disabled for device %s", device_id.c_str());
}

// =============================================================================
// Hotplug Registration
// =============================================================================

void NoteUSBSession::register_for_hotplug() {
    // Register session with global session manager for hotplug notifications
    SessionManager::instance().register_session(client_fd_, client_pid_, this);
    syslog(LOG_INFO, "NoteUSBSession: registered for hotplug notifications");
}

void NoteUSBSession::unregister_from_hotplug() {
    // Unregister from global session manager
    SessionManager::instance().unregister_session(client_pid_);
    syslog(LOG_INFO, "NoteUSBSession: unregistered from hotplug notifications");
}

// =============================================================================
// Device Operations
// =============================================================================

NoteDaemon::Error NoteUSBSession::discover_devices() {
    syslog(LOG_INFO, "NoteUSBSession: discovering devices for client pid=%d", client_pid_);

    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(usb_ctx_, &device_list);

    if (count < 0) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::UNKNOWN,
                                            "Failed to get device list: " +
                                            std::string(libusb_error_name((int)count)));
    }

    syslog(LOG_INFO, "NoteUSBSession: scanning %zd devices", count);

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = device_list[i];

        // Check if this is a HID device
        if (!is_hid_device(device)) {
            continue;
        }

        // Get device ID
        std::string device_id = get_device_id(device);

        // Check if we can open the device
        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == LIBUSB_SUCCESS) {
            // Build device descriptor
            device_ = build_device_descriptor(device, device_id);

            if (device_) {
                syslog(LOG_INFO, "NoteUSBSession: discovered device %s for client pid=%d",
                       device_id.c_str(), client_pid_);
                libusb_close(handle);
                break;  // Found first HID device
            }

            libusb_close(handle);
        }
    }

    libusb_free_device_list(device_list, 1);

    if (device_) {
        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    } else {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
                                            "No HID devices found");
    }
}

NoteDaemon::Error NoteUSBSession::claim_device(const std::string& device_id) {
    syslog(LOG_INFO, "NoteUSBSession: claiming device %s for client pid=%d",
           device_id.c_str(), client_pid_);

    // Find the device
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(usb_ctx_, &device_list);
    libusb_device* usb_device = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
        std::string current_id = get_device_id(device_list[i]);
        if (current_id == device_id) {
            usb_device = device_list[i];
            break;
        }
    }

    if (!usb_device) {
        libusb_free_device_list(device_list, 1);
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
                                            "Device not found: " + device_id);
    }

    // Open device
    libusb_device_handle* handle = nullptr;
    int result = libusb_open(usb_device, &handle);
    libusb_free_device_list(device_list, 1);

    if (result != LIBUSB_SUCCESS) {
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::PERMISSION_DENIED,
                                            "Cannot open device: " + device_id +
                                            " (" + std::string(libusb_error_name(result)) + ")");
    }

    // Claim interface (usually 0 for HID)
    result = libusb_claim_interface(handle, 0);
    if (result != LIBUSB_SUCCESS) {
        libusb_close(handle);
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::PERMISSION_DENIED,
                                            "Cannot claim interface: " + device_id +
                                            " (" + std::string(libusb_error_name(result)) + ")");
    }

    // Detach kernel driver if attached
    bool kernel_driver_attached = false;
    if (libusb_kernel_driver_active(handle, 0) == 1) {
        result = libusb_detach_kernel_driver(handle, 0);
        if (result == LIBUSB_SUCCESS) {
            kernel_driver_attached = true;
            syslog(LOG_INFO, "NoteUSBSession: detached kernel driver for device %s", device_id.c_str());
        } else {
            syslog(LOG_WARNING, "NoteUSBSession: failed to detach kernel driver: %s",
                   libusb_error_name(result));
        }
    }

    // Build device descriptor
    device_ = build_device_descriptor(usb_device, device_id);
    device_->handle = handle;
    device_->kernel_driver_attached = kernel_driver_attached;
    device_->owner_pid = client_pid_;  // Track who owns this device

    syslog(LOG_INFO, "NoteUSBSession: successfully claimed device %s for client pid=%d",
           device_id.c_str(), client_pid_);

    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

NoteDaemon::Error NoteUSBSession::release_device() {
    if (!device_ || !device_->handle) {
        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    std::string device_id = device_->device_id;
    syslog(LOG_INFO, "NoteUSBSession: releasing device %s for client pid=%d",
           device_id.c_str(), client_pid_);

    // Check if this client is the owner of the device
    if (device_->owner_pid != client_pid_) {
        syslog(LOG_ERR, "NoteUSBSession: client pid=%d is not owner of device %s (owner: %d)",
               client_pid_, device_id.c_str(), device_->owner_pid);
        return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::NOT_OWNER,
                                            "Not owner of device: " + device_id);
    }

    // Release interface
    libusb_release_interface(device_->handle, 0);

    // Reattach kernel driver if it was detached
    if (device_->kernel_driver_attached) {
        int result = libusb_attach_kernel_driver(device_->handle, 0);
        if (result == LIBUSB_SUCCESS) {
            syslog(LOG_INFO, "NoteUSBSession: reattached kernel driver for device %s", device_id.c_str());
        } else {
            syslog(LOG_WARNING, "NoteUSBSession: failed to reattach kernel driver: %s",
                   libusb_error_name(result));
        }
    }

    // Close handle
    libusb_close(device_->handle);
    device_->handle = nullptr;
    device_->kernel_driver_attached = false;

    syslog(LOG_INFO, "NoteUSBSession: successfully released device %s", device_id.c_str());

    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

void NoteUSBSession::send_response(const NoteBytes::Object& response) {
    // Check if socket is still valid before writing
    if (client_fd_ < 0) {
        syslog(LOG_WARNING, "NoteUSBSession: cannot send response - socket already closed (pid=%d)", client_pid_);
        return;
    }
    
    // Check if socket is still connected using poll
    struct pollfd pfd;
    pfd.fd = client_fd_;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    
    int poll_result = poll(&pfd, 1, 0);
    if (poll_result < 0) {
        syslog(LOG_WARNING, "NoteUSBSession: poll failed before send - socket error (pid=%d): %s", 
               client_pid_, strerror(errno));
        return;
    }
    if (poll_result == 0) {
        // Timeout with no events - socket might be in strange state, try anyway
        syslog(LOG_DEBUG, "NoteUSBSession: poll returned no events before send (pid=%d)", client_pid_);
    }
    
    try {
        NoteBytes::Writer writer(client_fd_, false);
        writer.write(response);
        writer.flush();
    } catch (const std::exception& e) {
        syslog(LOG_ERR, "NoteUSBSession: failed to send response (pid=%d): %s", 
               client_pid_, e.what());
        // Don't crash - just log the error
    }
}

void NoteUSBSession::send_error(const NoteBytes::Value& event, const std::string& device_id,
                                int code, const std::string& message) {
    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, event);
    msg.add(NoteMessaging::Keys::ERROR, code);
    msg.add(NoteMessaging::Keys::MSG, message);
    msg.add(NoteMessaging::Keys::DEVICE_ID, device_id);

    // Log the error first - this is important for debugging
    syslog(LOG_ERR, "NoteUSBSession: error %d: %s", code, message.c_str());
    
    // Now try to send the error response - but don't crash if it fails
    send_response(msg);
}

// =============================================================================
// Helper Functions
// =============================================================================

bool NoteUSBSession::is_hid_device(libusb_device* device) {
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

std::string NoteUSBSession::get_device_id(libusb_device* device) {
    uint8_t bus = libusb_get_bus_number(device);
    uint8_t address = libusb_get_device_address(device);
    return std::to_string(bus) + ":" + std::to_string(address);
}

std::shared_ptr<USBDeviceDescriptor> NoteUSBSession::build_device_descriptor(
    libusb_device* device, const std::string& device_id) {

    struct libusb_device_descriptor desc;
    int result = libusb_get_device_descriptor(device, &desc);
    if (result != LIBUSB_SUCCESS) {
        syslog(LOG_WARNING, "NoteUSBSession: failed to get device descriptor: %s",
               libusb_error_name(result));
        return nullptr;
    }

    auto device_desc = std::make_shared<USBDeviceDescriptor>();
    device_desc->device_id = device_id;
    device_desc->vendor_id = desc.idVendor;
    device_desc->product_id = desc.idProduct;
    device_desc->interface_number = 0;
    device_desc->kernel_driver_attached = false;
    device_desc->handle = nullptr;

    return device_desc;
}

void NoteUSBSession::start_streaming() {
    if (!device_ || !device_->handle) {
        syslog(LOG_ERR, "NoteUSBSession: cannot start streaming - no device handle");
        return;
    }

    // Create streaming thread
    streaming_thread_ = std::make_unique<HIDStreamingThread>(device_, device_id_, client_fd_);
    streaming_thread_->set_session(this);  // Enable encryption support
    streaming_thread_->start();

    syslog(LOG_INFO, "NoteUSBSession: streaming thread started for device %s", device_id_.c_str());
}

void NoteUSBSession::stop_streaming() {
    if (streaming_thread_) {
        streaming_thread_->stop();
        streaming_thread_.reset();
        syslog(LOG_INFO, "NoteUSBSession: streaming thread stopped for device %s", device_id_.c_str());
    }
}
// =============================================================================
// Encryption Operations
// =============================================================================

bool NoteUSBSession::encrypt_data(const std::vector<uint8_t>& plaintext,
                                  std::vector<uint8_t>& ciphertext) {
    if (!encryption_enabled_ || encryption_key_.empty()) {
        return false;
    }

    auto& encryption_provider = NoteDaemon::get_encryption_provider();
    
    // Use the device_id for encryption if we have one
    std::string device_id = device_id_.empty() ? "unknown" : device_id_;
    
    return encryption_provider.encrypt(device_id, plaintext, ciphertext);
}

bool NoteUSBSession::decrypt_data(const std::vector<uint8_t>& ciphertext,
                                  std::vector<uint8_t>& plaintext) {
    if (!encryption_enabled_ || encryption_key_.empty()) {
        return false;
    }

    auto& encryption_provider = NoteDaemon::get_encryption_provider();
    
    // Use the device_id for decryption if we have one
    std::string device_id = device_id_.empty() ? "unknown" : device_id_;
    
    return encryption_provider.decrypt(device_id, ciphertext, plaintext);
}
