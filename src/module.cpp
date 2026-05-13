// src/module.cpp
// NoteUSB module implementation - implements IModule interface

#include <string_view>
#include <vector>
#include <memory>
#include <map>
#include <thread>
#include <syslog.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <libusb-1.0/libusb.h>
#include <boost/multiprecision/cpp_int.hpp>

// Include NoteDaemon module framework headers
#include "module_framework/imodule.h"
#include "module_framework/handler_registry.h"
#include "module_framework/error.h"

#include "note_usb/device_handler.h"
#include "note_usb/note_usb_session.h"
#include "note_usb/session_manager.h"
#include "note_usb/note_usb_session.h"

// Include encryption API from NoteDaemon
#include "module_framework/encryption_api.h"

// Include NoteBytes and messaging
#include "note_messaging.h"
#include "notebytes_writer.h"
#include "event_bytes.h"
#include "notebytes.h"

using cpp_int = boost::multiprecision::cpp_int;

namespace NoteUSB {

/**
 * NoteUSB Module - implements IModule for USB/HID device handling
 * (Full class definition before factory function to avoid incomplete type error)
 */
class NoteUSBModule : public NoteDaemon::IModule {
public:
    NoteUSBModule()
        : handler_registry_(std::make_unique<NoteDaemon::HandlerRegistry>())
        , running_(false)
        , monitor_pid_(-1) {
    }

    ~NoteUSBModule() override {
        // Framework unload path already calls shutdown().
        // Only do destructor cleanup if resources are still live.
        if (running_ || device_handler_ || usb_ctx_ || !sessions_.empty()) {
            shutdown_with_reason("Destructor cleanup");
        }
    }

    // ===== IModule Implementation =====

    std::string_view name() const override {
        return "note_usb";
    }

    std::string_view version() const override {
        return "1.0.0";
    }

    std::string_view description() const override {
        return "USB/HID device support for NoteDaemon";
    }

    NoteDaemon::Error init(const nlohmann::json& config) override {
        syslog(LOG_INFO, "[NoteUSB] init() called");
        config_ = config;

        // Parse settings from config
        if (config.contains("settings")) {
            const auto& settings = config["settings"];
            discovery_interval_ms_ = settings.value("discovery_interval_ms", 1000);
            auto_detach_kernel_ = settings.value("auto_detach_kernel", true);
            usb_timeout_ms_ = settings.value("usb_timeout_ms", 100);
        }

        syslog(LOG_INFO, "[NoteUSB] init() - config loaded, about to init libusb");

        // Initialize libusb
        int rc = libusb_init(&usb_ctx_);
        if (rc != 0) {
            syslog(LOG_ERR, "[NoteUSB] init() - failed to init libusb: %s", libusb_error_name(rc));
            return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::MODULE_HEALTH_CHECK_FAILED,
                                                "Failed to initialize libusb");
        }

        syslog(LOG_INFO, "[NoteUSB] init() - libusb initialized successfully");

        // Initialize device handler
        syslog(LOG_INFO, "[NoteUSB] init() - creating device handler");
        device_handler_ = std::make_unique<DeviceHandler>();
        syslog(LOG_INFO, "[NoteUSB] init() - device handler created");

        syslog(LOG_INFO, "[NoteUSB] init() completed");

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    NoteDaemon::Error start() override {
        syslog(LOG_INFO, "[NoteUSB] start() called");
        if (running_) {
            syslog(LOG_WARNING, "[NoteUSB] start() called but already running");
            return NoteDaemon::Error(NoteDaemon::ErrorCodes::ALREADY_INITIALIZED,
                                      "Module already running");
        }

        syslog(LOG_INFO, "[NoteUSB] start() - starting device discovery");
        // Start device discovery
        if (device_handler_) {
            device_handler_->start_discovery();
        }
        syslog(LOG_INFO, "[NoteUSB] start() - device discovery started");

        // NOTE: Disabled unconditional module-level hotplug callbacks
        // Hotplug should only fire for claimed devices, not all USB devices
        // Per-session hotplug is handled in NoteUSBSession when a device is claimed
        // register_hotplug_callbacks();

        // Start device monitor if enabled
        if (config_.value("device_monitor", nlohmann::json::object())
                  .value("enabled", false)) {
            syslog(LOG_INFO, "[NoteUSB] start() - starting device monitor (disabled in config)");
            start_device_monitor();
        }

        // Register our handlers
        syslog(LOG_INFO, "[NoteUSB] start() - registering handlers");
        register_handlers(*handler_registry_);
        syslog(LOG_INFO, "[NoteUSB] start() - handlers registered");

        running_ = true;
        syslog(LOG_INFO, "[NoteUSB] start() - setting running=true");
        syslog(LOG_INFO, "[NoteUSB] start() completed");

        syslog(LOG_INFO, "NoteUSB: module started");

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    NoteDaemon::Error stop() override {
        // Log that stop() was called - interface requires this signature
        syslog(LOG_INFO, "[NoteUSB] stop() called (no reason provided)");
        
        // Call internal implementation with default reason
        return _stop_internal("Module stop() called (no reason provided)");
    }
    
    // Internal implementation that accepts a reason
    NoteDaemon::Error _stop_internal(const std::string& reason) {
        // Log why stop() is being called
        syslog(LOG_INFO, "[NoteUSB] _stop_internal() called! reason: %s", reason.c_str());
        
        if (!running_) {
            syslog(LOG_WARNING, "[NoteUSB] stop() called but module not running, reason: %s", reason.c_str());
            return NoteDaemon::Error(NoteDaemon::ErrorCodes::NOT_INITIALIZED,
                                      "Module not running");
        }

        // Stop device discovery
        if (device_handler_) {
            syslog(LOG_DEBUG, "[NoteUSB] stop() reason '%s' - stopping device discovery", reason.c_str());
            device_handler_->stop_discovery();
        }

        // Stop all sessions (release devices)
        syslog(LOG_DEBUG, "[NoteUSB] stop() reason '%s' - releasing all devices", reason.c_str());
        cleanup();

        // Note: Device monitor survives stop()
        running_ = false;
        syslog(LOG_INFO, "NoteUSB: module stopped (reason: %s)", reason.c_str());

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    void shutdown() override {
        shutdown_with_reason("Module framework requested shutdown");
    }
    
    // New function that accepts a reason for shutdown
    void shutdown_with_reason(const std::string& reason) {
        syslog(LOG_INFO, "NoteUSB: shutdown called with reason: %s", reason.c_str());
        
        _stop_internal(reason);
        
        // Clean up sessions
        sessions_.clear();
        
        // Clean up resources
        device_handler_.reset();
        
        // Cleanup libusb
        if (usb_ctx_) {
            libusb_exit(usb_ctx_);
            usb_ctx_ = nullptr;
        }
        
        syslog(LOG_INFO, "NoteUSB: module shutdown complete (reason: %s)", reason.c_str());
    }

    NoteDaemon::Error check_health(const std::string& core_api_version) override {
        // Check if we're compatible with the core API version
        if (core_api_version.substr(0, 2) != "1.") {
            return NoteDaemon::Error(NoteDaemon::ErrorCodes::MODULE_INCOMPATIBLE_VERSION,
                                     "Incompatible core API version");
        }

        // Check if libusb is available
        libusb_context* ctx = nullptr;
        int rc = libusb_init(&ctx);
        if (rc != 0) {
            return NoteDaemon::Error(NoteDaemon::ErrorCodes::MODULE_HEALTH_CHECK_FAILED,
                                     "Failed to initialize libusb");
        }
        libusb_exit(ctx);

        syslog(LOG_INFO, "NoteUSB: health check passed");
        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    cpp_int capabilities() const override {
        // USB/HID device capabilities
        cpp_int caps = 0;

        // Device types
        caps |= (cpp_int(1) << 34);  // USB_DEVICE (bit 34)
        caps |= (cpp_int(1) << 33);  // HID_DEVICE (bit 33)
        caps |= (cpp_int(1) << 32);  // DEVICE_TYPE_KNOWN (bit 32)

        // Modes
        caps |= (cpp_int(1) << 8);   // RAW_MODE (bit 8)
        caps |= (cpp_int(1) << 9);   // PARSED_MODE (bit 9)
        caps |= (cpp_int(1) << 11);  // FILTERED_MODE (bit 11)

        // Features
        caps |= (cpp_int(1) << 40);  // ENCRYPTION_SUPPORTED (bit 40)
        caps |= (cpp_int(1) << 42);  // BUFFERING_SUPPORTED (bit 42)
        caps |= (cpp_int(1) << 28);  // NANOSECOND_TIMESTAMPS (bit 28)

        return caps;
    }

    std::vector<std::string> get_handled_message_types() override {
        return {
            "request_discovery",
            "claim_item",
            "release_item",
            "resume",
            "device_disconnected"
        };
    }

    NoteDaemon::HandlerRegistry& get_handler_registry() override {
        return *handler_registry_;
    }

    void collect_errors(std::vector<NoteDaemon::Error>& errors) override {
        if (device_handler_) {
            device_handler_->collect_errors(errors);
        }
    }

    void cleanup() override {
        if (device_handler_) {
            device_handler_->release_all_devices();
        }

        // Clear session registry
        sessions_.clear();
    }

    // ===== Device Discovery State =====

    void discover_devices() {
        if (device_handler_) {
            device_handler_->discover_devices();
        }
    }

    NoteDaemon::Error claim_device(const NoteBytes::Object& msg) {
        if (device_handler_) {
            return device_handler_->claim_device(msg);
        }
        return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::NOT_INITIALIZED,
                                            "Device handler not initialized");
    }

    NoteDaemon::Error release_device(const NoteBytes::Object& msg) {
        if (device_handler_) {
            return device_handler_->release_device(msg);
        }
        return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::NOT_INITIALIZED,
                                            "Device handler not initialized");
    }

private:
    void register_handlers(NoteDaemon::HandlerRegistry& registry) {
        // Register handlers for message types

        // Discovery request
        registry.register_handler(NoteMessaging::ProtocolMessages::REQUEST_DISCOVERY, [this](const NoteBytes::Object& msg) {
            (void)msg;  // Suppress unused warning
            syslog(LOG_DEBUG, "NoteUSB: handle request_discovery");
            send_device_list();
        });

        // Claim item
        registry.register_handler(NoteMessaging::ProtocolMessages::CLAIM_ITEM, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: handle claim_item");
            handle_claim_device(msg);
        });

        // Release item
        registry.register_handler(NoteMessaging::ProtocolMessages::RELEASE_ITEM, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: handle release_item");
            handle_release_device(msg);
        });

        // Resume
        registry.register_handler(NoteMessaging::ProtocolMessages::RESUME, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: handle resume");
            handle_resume(msg);
        });

        syslog(LOG_INFO, "NoteUSB: registered %zu handlers", registry.handler_count());
    }

    void start_device_monitor() {
        std::string monitor_path = "/etc/netnotes/modules/note_usb/note_usb_monitor";

        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "NoteUSB: failed to fork device monitor: %s", strerror(errno));
            return;
        }

        if (pid == 0) {
            // Child - exec the monitor
            char pid_str[32];
            snprintf(pid_str, sizeof(pid_str), "%d", getppid());

            execl(monitor_path.c_str(), "note_usb_monitor", pid_str, nullptr);

            syslog(LOG_ERR, "NoteUSB: failed to exec monitor: %s", strerror(errno));
            _exit(1);
        }

        monitor_pid_ = pid;
        syslog(LOG_INFO, "NoteUSB: device monitor started (PID %d)", pid);
    }

private:
    // ===== DEVICE HANDLERS =====

    void send_device_list() {
        // Ensure devices are discovered first
        if (device_handler_) {
            device_handler_->discover_devices();
        }

        NoteBytes::Object response;
        response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_LIST);

        NoteBytes::Array devices_array;
        if (device_handler_) {
            const auto& devices = device_handler_->available_devices();
            for (const auto& [id, device] : devices) {
                auto device_obj = device->to_notebytes();
                auto device_bytes = device_obj.serialize();
                devices_array.add(NoteBytes::Value(device_bytes, NoteBytes::Type::OBJECT));
            }
        }
        response.add(NoteMessaging::Keys::ITEMS, devices_array.as_value());

        send_response(response);
        syslog(LOG_INFO, "NoteUSB: sent device list");
    }

    void handle_claim_device(const NoteBytes::Object& msg) {
        // Delegate to DeviceHandler
        NoteDaemon::Error result = NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::UNKNOWN, "Device handler not initialized");

        if (device_handler_) {
            result = device_handler_->claim_device(msg);
        }

        if (result.success()) {
            // Get device_id from message
            auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
            std::string device_id = device_id_val ? device_id_val->as_string() : "";

            // Send success response
            NoteBytes::Object response;
            response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_CLAIMED);
            response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
            response.add(NoteMessaging::Keys::STATUS, "claimed");

            send_response(response);
            syslog(LOG_INFO, "NoteUSB: device claimed: %s", device_id.c_str());
        } else {
            // Send error response
            auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
            std::string device_id = device_id_val ? device_id_val->as_string() : "";

            send_error(NoteMessaging::ProtocolMessages::ITEM_CLAIMED, device_id,
                      result.code, std::string(result.message()));
        }
    }

    void handle_release_device(const NoteBytes::Object& msg) {
        // Delegate to DeviceHandler
        NoteDaemon::Error result = NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::UNKNOWN, "Device handler not initialized");

        if (device_handler_) {
            result = device_handler_->release_device(msg);
        }

        if (result.success()) {
            // Get device_id from message
            auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
            std::string device_id = device_id_val ? device_id_val->as_string() : "";

            // Send success response
            NoteBytes::Object response;
            response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ITEM_RELEASED);
            response.add(NoteMessaging::Keys::DEVICE_ID, device_id);
            response.add(NoteMessaging::Keys::STATUS, NoteMessaging::ProtocolMessages::SUCCESS);

            send_response(response);
            syslog(LOG_INFO, "NoteUSB: device released: %s", device_id.c_str());
        } else {
            // Send error response
            auto* device_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
            std::string device_id = device_id_val ? device_id_val->as_string() : "";

            send_error(NoteMessaging::ProtocolMessages::ITEM_RELEASED, device_id,
                      result.code, std::string(result.message()));
        }
    }

    void handle_resume(const NoteBytes::Object& msg) {
        int processed_count = msg.get_int(std::string_view("processed_count"), 0);
        std::string device_id = msg.get_string(NoteMessaging::Keys::DEVICE_ID, NoteMessaging::Keys::EMPTY);

        if (device_id.empty()) {
            syslog(LOG_WARNING, "Resume message missing device_id");
            return;
        }

        syslog(LOG_DEBUG, "NoteUSB: resume acknowledged %d messages for device %s",
               processed_count, device_id.c_str());

        if (processed_count <= 0) return;

        // TODO: Implement resume logic
        syslog(LOG_DEBUG, "NoteUSB: resume for device %s", device_id.c_str());
    }

    void send_response(const NoteBytes::Object& response) {
        // Get client_fd from somewhere - this is a placeholder
        // In a real implementation, we'd track active sessions
        syslog(LOG_DEBUG, "NoteUSB: send_response - not yet implemented");
    }

    void send_error(const NoteBytes::Value& event, const std::string& device_id,
                    int code, const std::string& message) {
        NoteBytes::Object msg;
        msg.add(NoteMessaging::Keys::EVENT, event);
        msg.add(NoteMessaging::Keys::ERROR, code);
        msg.add(NoteMessaging::Keys::MSG, message);
        msg.add(NoteMessaging::Keys::DEVICE_ID, device_id);

        send_response(msg);
    }

    void send_error(int code, const std::string& message) {
        NoteBytes::Object msg;
        msg.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ERROR);
        msg.add(NoteMessaging::Keys::ERROR, code);
        msg.add(NoteMessaging::Keys::MSG, message);

        send_response(msg);
    }

    // ===== SESSION MANAGEMENT =====

    NoteDaemon::Error handle_client(int client_fd, pid_t client_pid) override {
        syslog(LOG_INFO, "NoteUSB: handle_client called (pid=%d, fd=%d)", client_pid, client_fd);

        // Check if session already exists
        if (sessions_.count(client_pid) > 0) {
            std::string msg = "Session already exists for pid=" + std::to_string(client_pid);
            return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::ALREADY_INITIALIZED,
                                                msg);
        }

        // Create session (discovery only - no device yet)
        auto session = std::make_unique<NoteUSBSession>(usb_ctx_, client_fd, client_pid);
        
        // IMPORTANT: Start session reading in a separate thread
        // The session->start() method runs read_socket() in a blocking loop
        // If we call it in this thread, the code after it (sending ACCEPT) 
        // would never execute!
        // Note: We capture session.get() (raw pointer) before moving the unique_ptr
        NoteUSBSession* session_ptr = session.get();
        std::thread session_thread([session_ptr, client_pid]() {
            syslog(LOG_INFO, "NoteUSB: starting session thread for pid=%d", client_pid);
            session_ptr->start();
            syslog(LOG_INFO, "NoteUSB: session thread ended for pid=%d", client_pid);
        });
        
        // Detach the thread - session will clean itself up when socket closes
        session_thread.detach();

        // Store session (transfer ownership using move)
        sessions_[client_pid] = std::move(session);

        syslog(LOG_INFO, "NoteUSB: session created and started (pid=%d, total=%zu)",
               client_pid, sessions_.size());

        // Send ACCEPT response for handshake
        // This is now sent AFTER the session reading has started in background
        try {
            NoteBytes::Object accept_response;
            accept_response.add(NoteMessaging::Keys::EVENT, NoteMessaging::ProtocolMessages::ACCEPT);
            accept_response.add(NoteMessaging::Keys::STATUS, "accepted");
            
            NoteBytes::Writer writer(client_fd, false);
            writer.write(accept_response);
            writer.flush();
            
            syslog(LOG_INFO, "NoteUSB: sent ACCEPT to pid=%d", client_pid);
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "NoteUSB: failed to send ACCEPT to pid=%d: %s", 
                   client_pid, e.what());
            // Don't fail the connection - session will handle this
        }

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    void cleanup_client(pid_t client_pid) override {
        syslog(LOG_INFO, "NoteUSB: cleanup_client called (pid=%d)", client_pid);

        auto it = sessions_.find(client_pid);
        if (it != sessions_.end()) {
            it->second->stop();
            sessions_.erase(it);
            syslog(LOG_INFO, "NoteUSB: session cleaned up (pid=%d, total=%zu)",
                   client_pid, sessions_.size());
        }
    }

    void register_session(pid_t client_pid, int client_fd) {
        SessionManager::instance().register_session(client_fd, client_pid, nullptr);
        syslog(LOG_INFO, "NoteUSB: session registered (pid=%d, fd=%d, total=%zu)",
               client_pid, client_fd, SessionManager::instance().session_count());
    }

    void unregister_session(pid_t client_pid) {
        SessionManager::instance().unregister_session(client_pid);
        syslog(LOG_INFO, "NoteUSB: session unregistered (pid=%d, total=%zu)",
               client_pid, SessionManager::instance().session_count());
    }

    void broadcast_to_all_sessions(const NoteBytes::Object& msg) {
        SessionManager::instance().broadcast_to_all_sessions(msg);
    }

    // ===== HOTPLUG SUPPORT =====
    
    void register_hotplug_callbacks() {
        if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
            syslog(LOG_WARNING, "NoteUSB: libusb hotplug not supported on this platform");
            return;
        }

        libusb_hotplug_callback_handle handle_attached, handle_detached;

        // Register for device arrivals
        int rc = libusb_hotplug_register_callback(
            usb_ctx_,
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
            LIBUSB_HOTPLUG_ENUMERATE,  // Also fire for existing devices
            LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback_attached,
            this,
            &handle_attached
        );

        if (rc != LIBUSB_SUCCESS) {
            syslog(LOG_ERR, "NoteUSB: failed to register hotplug callback (attached): %s",
                   libusb_error_name(rc));
        } else {
            syslog(LOG_INFO, "NoteUSB: registered hotplug callback for USB device arrivals");
        }

        // Register for device departures
        rc = libusb_hotplug_register_callback(
            usb_ctx_,
            LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
            LIBUSB_HOTPLUG_NO_FLAGS,
            LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY,
            LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback_detached,
            this,
            &handle_detached
        );

        if (rc != LIBUSB_SUCCESS) {
            syslog(LOG_ERR, "NoteUSB: failed to register hotplug callback (detached): %s",
                   libusb_error_name(rc));
        } else {
            syslog(LOG_INFO, "NoteUSB: registered hotplug callback for USB device departures");
        }
    }

    static int LIBUSB_CALL hotplug_callback_attached(
        libusb_context* ctx,
        libusb_device* device,
        libusb_hotplug_event event,
        void* user_data)
    {
        (void)ctx;         // unused
        (void)event;       // unused
        (void)user_data;  // unused

        auto* self = static_cast<NoteUSBModule*>(user_data);

        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

            syslog(LOG_INFO, "NoteUSB: USB device attached: %s", device_id.c_str());

            // Build descriptor for this device
            auto device_desc = self->build_device_descriptor(device, device_id);
            if (!device_desc) {
                return 0;  // Not a HID device, ignore
            }

            // Send DEVICE_ATTACHED to all sessions
            self->send_device_attached(device_id, device_desc);
        }

        return 0;  // Continue receiving events
    }

    static int LIBUSB_CALL hotplug_callback_detached(
        libusb_context* ctx,
        libusb_device* device,
        libusb_hotplug_event event,
        void* user_data)
    {
        (void)ctx;         // unused
        (void)event;       // unused
        (void)user_data;  // unused

        auto* self = static_cast<NoteUSBModule*>(user_data);

        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

            syslog(LOG_INFO, "NoteUSB: USB device detached: %s", device_id.c_str());

            // Send DEVICE_DETACHED to all sessions
            self->send_device_detached(device_id);
        }

        return 0;  // Continue receiving events
    }

    std::shared_ptr<USBDeviceDescriptor> build_device_descriptor(
        libusb_device* device, const std::string& device_id)
    {
        struct libusb_device_descriptor desc;
        int result = libusb_get_device_descriptor(device, &desc);
        if (result != LIBUSB_SUCCESS) {
            syslog(LOG_WARNING, "NoteUSB: failed to get device descriptor for %s: %s",
                   device_id.c_str(), libusb_error_name(result));
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

    void send_device_attached(const std::string& device_id,
                              const std::shared_ptr<USBDeviceDescriptor>& device_desc)
    {
        NoteBytes::Object notification;
        notification.add(NoteMessaging::Keys::EVENT,
                        NoteMessaging::ProtocolMessages::DEVICE_ATTACHED);
        notification.add(NoteMessaging::Keys::DEVICE_ID, device_id);

        // Include full device descriptor (same format as ITEM_LIST entries)
        auto device_obj = device_desc->to_notebytes();
        notification.add(NoteMessaging::ProtocolMessages::ITEM_INFO, device_obj.as_value());

        broadcast_to_all_sessions(notification);

        syslog(LOG_INFO, "NoteUSB: sent DEVICE_ATTACHED for %s to all sessions",
               device_id.c_str());
    }

    void send_device_detached(const std::string& device_id) {
        NoteBytes::Object notification;
        notification.add(NoteMessaging::Keys::EVENT,
                        NoteMessaging::ProtocolMessages::DEVICE_DETACHED);
        notification.add(NoteMessaging::Keys::DEVICE_ID, device_id);

        broadcast_to_all_sessions(notification);

        syslog(LOG_INFO, "NoteUSB: sent DEVICE_DETACHED for %s to all sessions",
               device_id.c_str());
    }

    // Configuration
    nlohmann::json config_;
    int discovery_interval_ms_ = 1000;
    bool auto_detach_kernel_ = true;
    int usb_timeout_ms_ = 100;

    // State
    bool running_;
    pid_t monitor_pid_;

    // Components
    std::unique_ptr<NoteDaemon::HandlerRegistry> handler_registry_;
    std::unique_ptr<DeviceHandler> device_handler_;
    libusb_context* usb_ctx_ = nullptr;

    // Active sessions (pid -> NoteUSBSession)
    std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;

    // Note: Device state is managed by DeviceHandler, not here
    // This avoids duplication between module and handler
};

// Module factory function - exported for dynamic loading
// Must come AFTER the full class definition to avoid incomplete type error
extern "C" NoteDaemon::IModule* create_module() {
    static NoteUSBModule instance;
    return &instance;
}

} // namespace NoteUSB
