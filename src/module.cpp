// src/module.cpp
// NoteUSB module implementation - implements IModule interface
// Two-socket architecture: management socket handles claim/release/discovery
// Device socket handles HID event streaming

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
#include "module_framework/device_ownership_registry.h"
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
        , monitor_pid_(-1)
        , ownership_registry_(nullptr)
    {
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

        // Initialize device handler - NOTE: ownership_registry_ will be set
        // via set_ownership_registry() after init() returns
        syslog(LOG_INFO, "[NoteUSB] init() - creating device handler (without ownership registry initially)");
        device_handler_ = std::make_unique<DeviceHandler>(nullptr, "note_usb");
        syslog(LOG_INFO, "[NoteUSB] init() - device handler created");

        syslog(LOG_INFO, "[NoteUSB] init() completed");

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    void set_ownership_registry(NoteDaemon::DeviceOwnershipRegistry* registry) override {
        syslog(LOG_INFO, "[NoteUSB] set_ownership_registry() called: %p", (void*)registry);
        ownership_registry_ = registry;
        
        // Re-create DeviceHandler with ownership registry
        // This is called after init() returns successfully
        if (device_handler_) {
            device_handler_ = std::make_unique<DeviceHandler>(ownership_registry_, "note_usb");
            syslog(LOG_INFO, "[NoteUSB] DeviceHandler re-created with ownership registry");
        }
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

        // Start device monitor if enabled
        if (config_.value("device_monitor", nlohmann::json::object())
                  .value("enabled", false)) {
            syslog(LOG_INFO, "[NoteUSB] start() - starting device monitor (disabled in config)");
            start_device_monitor();
        }

        // Register our handlers (for internal dispatch, not used for two-socket)
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

    // ── Two-socket: Management message handler ────────────────────────────────

    NoteDaemon::Error handle_management_message(const NoteBytes::Object& message,
                                                int reply_fd,
                                                pid_t client_pid) override {
        // Determine command type
        auto* cmd_val = message.get(NoteMessaging::Keys::CMD);
        if (!cmd_val) {
            // Try EVENT field
            cmd_val = message.get(NoteMessaging::Keys::EVENT);
        }
        
        if (!cmd_val) {
            syslog(LOG_WARNING, "[NoteUSB] handle_management_message: no CMD or EVENT field");
            return NoteDaemon::Error::from_code(NoteMessaging::ErrorCodes::INVALID_MESSAGE,
                                               "Missing CMD or EVENT field");
        }

        // Get command as string for logging (but compare NoteBytes::Value directly)
        std::string cmd = cmd_val->as_string();
        syslog(LOG_DEBUG, "[NoteUSB] handle_management_message: cmd=%s pid=%d", cmd.c_str(), client_pid);

        // NOTE: We use the actual client_pid from SO_PEERCRED, not anything
        // from the message. This prevents clients from impersonating other PIDs.

        // Compare NoteBytes::Value directly instead of converting to string
        if (*cmd_val == NoteMessaging::ProtocolMessages::REQUEST_DISCOVERY) {
            // Handle discovery request
            if (device_handler_) {
                device_handler_->send_device_list(reply_fd);
            }
            return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
        }

        if (*cmd_val == NoteMessaging::ProtocolMessages::CLAIM_ITEM) {
            // Handle claim request - use ACTUAL client_pid from socket credentials
            if (!device_handler_) {
                return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::NOT_INITIALIZED,
                                                    "Device handler not initialized");
            }
            return device_handler_->claim_device(message, reply_fd, client_pid);
        }

        if (*cmd_val == NoteMessaging::ProtocolMessages::RELEASE_ITEM) {
            // Handle release request
            if (!device_handler_) {
                return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::NOT_INITIALIZED,
                                                    "Device handler not initialized");
            }
            return device_handler_->release_device(message, reply_fd);
        }

        syslog(LOG_WARNING, "[NoteUSB] handle_management_message: unknown command: %s", cmd.c_str());
        return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::UNKNOWN,
                                            "Unknown command: " + cmd);
    }

    // ── Two-socket: Device socket handler (post-DEVICE_HANDSHAKE) ─────────────

    NoteDaemon::Error handle_client(int client_fd, pid_t client_pid) override {
        syslog(LOG_INFO, "NoteUSB: handle_client (device socket) called (pid=%d, fd=%d)", 
               client_pid, client_fd);

        // Check if session already exists
        if (sessions_.count(client_pid) > 0) {
            std::string msg = "Session already exists for pid=" + std::to_string(client_pid);
            return NoteDaemon::Error::from_code(NoteDaemon::ErrorCodes::ALREADY_INITIALIZED,
                                                msg);
        }

        // Create session - this session reads HID events from the device socket
        auto session = std::make_unique<NoteUSBSession>(usb_ctx_, client_fd, client_pid);
        
        // Start session reading in a separate thread
        NoteUSBSession* session_ptr = session.get();
        std::thread session_thread([session_ptr, client_pid]() {
            syslog(LOG_INFO, "NoteUSB: starting device socket session thread for pid=%d", client_pid);
            session_ptr->start();
            syslog(LOG_INFO, "NoteUSB: device socket session thread ended for pid=%d", client_pid);
        });
        
        // Detach the thread - session will clean itself up when socket closes
        session_thread.detach();

        // Store session
        sessions_[client_pid] = std::move(session);

        syslog(LOG_INFO, "NoteUSB: device socket session created (pid=%d, total=%zu)",
               client_pid, sessions_.size());

        // Note: We don't send ACCEPT here - the device socket is for streaming events
        // The management socket handles the initial handshake and claim response

        return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
    }

    void cleanup_client(pid_t client_pid) override {
        syslog(LOG_INFO, "NoteUSB: cleanup_client (device socket) called (pid=%d)", client_pid);

        auto it = sessions_.find(client_pid);
        if (it != sessions_.end()) {
            it->second->stop();
            sessions_.erase(it);
            syslog(LOG_INFO, "NoteUSB: device socket session cleaned up (pid=%d, total=%zu)",
                   client_pid, sessions_.size());
        }
    }

    // ===== Remaining IModule methods =====

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

private:
    void register_handlers(NoteDaemon::HandlerRegistry& registry) {
        // Register handlers for message types (kept for internal use, not socket routing)

        // Discovery request
        registry.register_handler(NoteMessaging::ProtocolMessages::REQUEST_DISCOVERY, [this](const NoteBytes::Object& msg) {
            (void)msg;
            syslog(LOG_DEBUG, "NoteUSB: internal handler request_discovery");
            if (device_handler_) {
                device_handler_->send_device_list(-1);  // broadcast
            }
        });

        // Claim item
        registry.register_handler(NoteMessaging::ProtocolMessages::CLAIM_ITEM, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: internal handler claim_item");
            // This path is no longer used - handle_management_message handles it
        });

        // Release item
        registry.register_handler(NoteMessaging::ProtocolMessages::RELEASE_ITEM, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: internal handler release_item");
            // This path is no longer used - handle_management_message handles it
        });

        // Resume
        registry.register_handler(NoteMessaging::ProtocolMessages::RESUME, [this](const NoteBytes::Object& msg) {
            syslog(LOG_DEBUG, "NoteUSB: internal handler resume");
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

    // ===== Legacy/internal handlers (not used for two-socket) =====

    void handle_resume(const NoteBytes::Object& msg) {
        int processed_count = msg.get_int(std::string_view("processed_count"), 0);
        std::string device_id = msg.get_string(NoteMessaging::Keys::DEVICE_ID, NoteMessaging::Keys::EMPTY);

        if (device_id.empty()) {
            syslog(LOG_WARNING, "Resume message missing device_id");
            return;
        }

        syslog(LOG_DEBUG, "NoteUSB: resume acknowledged %d messages for device %s",
               processed_count, device_id.c_str());

        // TODO: Implement resume logic
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
            LIBUSB_HOTPLUG_ENUMERATE,
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
        }
    }

    static int LIBUSB_CALL hotplug_callback_attached(
        libusb_context* ctx,
        libusb_device* device,
        libusb_hotplug_event event,
        void* user_data)
    {
        (void)ctx; (void)event; (void)user_data;

        auto* self = static_cast<NoteUSBModule*>(user_data);

        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

            syslog(LOG_INFO, "NoteUSB: USB device attached: %s", device_id.c_str());

            auto device_desc = self->build_device_descriptor(device, device_id);
            if (!device_desc) {
                return 0;
            }

            // Broadcast to management sockets
            self->send_device_attached(device_id, device_desc);
        }

        return 0;
    }

    static int LIBUSB_CALL hotplug_callback_detached(
        libusb_context* ctx,
        libusb_device* device,
        libusb_hotplug_event event,
        void* user_data)
    {
        (void)ctx; (void)event; (void)user_data;

        auto* self = static_cast<NoteUSBModule*>(user_data);

        if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

            syslog(LOG_INFO, "NoteUSB: USB device detached: %s", device_id.c_str());

            self->send_device_detached(device_id);
        }

        return 0;
    }

    std::shared_ptr<USBDeviceDescriptor> build_device_descriptor(
        libusb_device* device, const std::string& device_id)
    {
        struct libusb_device_descriptor desc;
        int result = libusb_get_device_descriptor(device, &desc);
        if (result != LIBUSB_SUCCESS) {
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

        auto device_obj = device_desc->to_notebytes();
        notification.add(NoteMessaging::ProtocolMessages::ITEM_INFO, device_obj.as_value());

        // Broadcast to management sockets via SessionManager
        SessionManager::instance().broadcast_to_all(notification);
    }

    void send_device_detached(const std::string& device_id) {
        NoteBytes::Object notification;
        notification.add(NoteMessaging::Keys::EVENT,
                        NoteMessaging::ProtocolMessages::DEVICE_DETACHED);
        notification.add(NoteMessaging::Keys::DEVICE_ID, device_id);

        // Broadcast to management sockets via SessionManager
        SessionManager::instance().broadcast_to_all(notification);
    }

    // Configuration
    nlohmann::json config_;
    int discovery_interval_ms_ = 1000;
    bool auto_detach_kernel_ = true;
    int usb_timeout_ms_ = 100;

    // State
    bool running_;
    pid_t monitor_pid_;

    // Core-provided registry for device ownership
    NoteDaemon::DeviceOwnershipRegistry* ownership_registry_;

    // Components
    std::unique_ptr<NoteDaemon::HandlerRegistry> handler_registry_;
    std::unique_ptr<DeviceHandler> device_handler_;
    libusb_context* usb_ctx_ = nullptr;

    // Active device socket sessions (pid -> NoteUSBSession)
    std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;
};

// Module factory function - exported for dynamic loading
extern "C" NoteDaemon::IModule* create_module() {
    static NoteUSBModule instance;
    return &instance;
}

} // namespace NoteUSB