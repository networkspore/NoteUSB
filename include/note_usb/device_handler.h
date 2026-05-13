// include/note_usb/device_handler.h
// DeviceHandler – USB device discovery, claim, and release for NoteUSB.
//
// Two-socket changes:
//   • claim_device() and release_device() now accept a reply_fd so they can
//     write ITEM_CLAIMED / ITEM_RELEASED directly on the management socket.
//   • On successful claim, DeviceHandler calls
//     ownership_registry_->register_device() so the core can route the
//     subsequent DEVICE_HANDSHAKE connection to this module.
//   • On release, it calls ownership_registry_->unregister_device().

#ifndef NOTE_USB_DEVICE_HANDLER_H
#define NOTE_USB_DEVICE_HANDLER_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <libusb-1.0/libusb.h>
#include <boost/multiprecision/cpp_int.hpp>

#include "module_framework/error.h"
#include "module_framework/device_ownership_registry.h"
#include "notebytes.h"

namespace NoteUSB {

using cpp_int = boost::multiprecision::cpp_int;

// ── USB device descriptors ────────────────────────────────────────────────────

struct USBDeviceDescriptor {
    std::string           device_id;             // "bus:address", e.g. "1:2"
    uint16_t              vendor_id           = 0;
    uint16_t              product_id          = 0;
    int                   interface_number    = 0;
    bool                  kernel_driver_attached = false;
    libusb_device_handle* handle              = nullptr; // set when claimed
    pid_t                 owner_pid           = 0;       // claiming client PID

    NoteBytes::Object to_notebytes() const;
};

/** Entry written to the crash-recovery registry file. */
struct ClaimedDevice {
    pid_t       pid                    = 0;
    std::string device_id;
    int         interface_number       = 0;
    bool        kernel_driver_attached = false;
};

// ── DeviceRegistry (crash-recovery file) ─────────────────────────────────────

/**
 * Persists claimed devices to /run/netnotes/modules/note_usb/device_registry.json
 * so a monitor process can reattach kernel drivers after a daemon crash.
 */
class DeviceRegistry {
public:
    static const std::string& path();

    static void add_device(const std::string& device_id,
                           int interface_number,
                           bool kernel_driver_attached);
    static bool remove_device(const std::string& device_id);
    static std::vector<ClaimedDevice> get_all_devices();
    static void remove_all();

private:
    static std::mutex&                  registry_mutex();
    static std::string&                 registry_path();
    static std::vector<ClaimedDevice>   read_registry();
    static bool                         write_registry(const std::vector<ClaimedDevice>&);
    static std::string                  json_object(const ClaimedDevice&);
};

// ── DeviceHandler ─────────────────────────────────────────────────────────────

class DeviceHandler {
public:
    explicit DeviceHandler(NoteDaemon::DeviceOwnershipRegistry* ownership_registry,
                           std::string_view module_name = "note_usb");
    ~DeviceHandler();

    // Discovery
    void start_discovery();
    void stop_discovery();
    void discover_devices();

    /**
     * Send the current device list as an ITEM_LIST message on reply_fd.
     * If reply_fd == -1, broadcast to all sessions via SessionManager.
     */
    void send_device_list(int reply_fd);

    /**
     * Claim a USB device.
     *
     * On success:
     *   • Writes ITEM_CLAIMED to reply_fd
     *   • Registers device_id → module_name in ownership_registry_
     *
     * On failure:
     *   • Writes an error response to reply_fd
     *
     * @param msg       Incoming CLAIM_ITEM management message.
     * @param reply_fd  Management socket fd to write the response to.
     * @param client_pid PID of the claiming client (stored in DeviceInfo).
     */
    NoteDaemon::Error claim_device(const NoteBytes::Object& msg,
                                   int reply_fd,
                                   pid_t client_pid);

    /**
     * Release a claimed USB device.
     *
     * On success:
     *   • Writes ITEM_RELEASED to reply_fd
     *   • Unregisters device_id from ownership_registry_
     */
    NoteDaemon::Error release_device(const NoteBytes::Object& msg,
                                     int reply_fd);

    /** Release every claimed device (shutdown / cleanup path). */
    void release_all_devices();

    /** Append any pending errors to @p errors. */
    void collect_errors(std::vector<NoteDaemon::Error>& errors);

    // ── Accessors ──────────────────────────────────────────────────────────────

    struct DeviceInfo {
        std::string           device_id;
        libusb_device_handle* handle              = nullptr;
        int                   interface_number    = 0;
        bool                  kernel_driver_attached = false;
        cpp_int               capabilities;
        pid_t                 owner_pid           = 0;
    };

    const std::map<std::string, std::shared_ptr<USBDeviceDescriptor>>&
        available_devices() const { return available_devices_; }

    const std::map<std::string, DeviceInfo>&
        claimed_devices()   const { return claimed_devices_; }

    // ── Helpers ────────────────────────────────────────────────────────────────

    bool is_hid_device(libusb_device* device);
    std::shared_ptr<USBDeviceDescriptor> find_device_by_id(const std::string& id);

private:
    /** Write a NoteBytes::Object to fd.  Logs but does not throw on failure. */
    void write_to_fd(int fd, const NoteBytes::Object& obj);

    /** Send an error response on the management socket. */
    void send_error_response(int fd, const NoteBytes::Value& event,
                              const std::string& device_id,
                              int code, const std::string& message);

    libusb_context* ctx_              = nullptr;
    bool            running_          = false;

    NoteDaemon::DeviceOwnershipRegistry* ownership_registry_ = nullptr;
    std::string module_name_;

    std::map<std::string, std::shared_ptr<USBDeviceDescriptor>> available_devices_;
    std::map<std::string, DeviceInfo>                           claimed_devices_;
};

} // namespace NoteUSB

#endif // NOTE_USB_DEVICE_HANDLER_H