// include/note_usb/note_usb_session.h
// NoteUSB session - manages one device per connection
// Each NoteUSBSession is dedicated to one client and one device

#ifndef NOTE_USB_SESSION_H
#define NOTE_USB_SESSION_H

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <syslog.h>
#include <libusb-1.0/libusb.h>
#include "notebytes.h"
#include "note_messaging.h"
#include "event_bytes.h"
#include "usb_device_descriptor.h"
#include "hid_streaming_thread.h"
#include "module_framework/error.h"

namespace NoteUSB {

/**
 * NoteUSB Session - manages one device for one client connection
 *
 * Design: One session = one device = one client
 * - Session is created when client connects
 * - Session claims one device
 * - Session streams events to client
 * - Session is destroyed when client disconnects
 */
class NoteUSBSession {
public:
    /**
     * Create a NoteUSB session
     * @param usb_ctx libusb context
     * @param client_fd Client socket file descriptor
     * @param client_pid Client process ID
     * @param device_id Device to claim (empty = discovery only)
     */
    NoteUSBSession(libusb_context* usb_ctx, int client_fd, pid_t client_pid,
                   const std::string& device_id = "");
    
    // Register for hotplug notifications
    void register_for_hotplug();
    void unregister_from_hotplug();

    ~NoteUSBSession();

    // Session lifecycle
    void start();
    void stop();
    bool is_running() const;
    bool is_encryption_enabled() const { return encryption_enabled_; }

    // Message handling
    void handle_message(const NoteBytes::Object& message);
    void read_socket();

private:
    // ===== State =====
    libusb_context* usb_ctx_;
    int client_fd_;
    pid_t client_pid_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // ===== Device Management =====
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;

    // ===== Encryption =====
    bool encryption_enabled_ = false;
    std::vector<uint8_t> encryption_key_;

    // ===== Streaming =====
    std::unique_ptr<HIDStreamingThread> streaming_thread_;

    // ===== Device State =====
    std::atomic<int> events_dropped_{0};
    std::atomic<int> events_queued_{0};
    std::atomic<int> events_delivered_{0};
    
    // ===== Metrics =====
    std::atomic<uint64_t> events_read_{0};
    std::atomic<uint64_t> events_sent_{0};

    // ===== Message Handlers =====
    void handle_discover(const NoteBytes::Object& message);
    void handle_claim(const NoteBytes::Object& message);
    void handle_release(const NoteBytes::Object& message);
    void handle_resume(const NoteBytes::Object& message);
    void handle_enable_encryption(const NoteBytes::Object& message);
    void handle_disable_encryption(const NoteBytes::Object& message);

    // ===== Device Operations =====
    NoteDaemon::Error discover_devices();
    NoteDaemon::Error claim_device(const std::string& device_id);
    NoteDaemon::Error release_device();
    void start_streaming();
    void stop_streaming();
    void send_response(const NoteBytes::Object& response);
    void send_error(const NoteBytes::Value& event, const std::string& device_id,
                    int code, const std::string& message);

public:
    // ===== Encryption Operations =====
    bool encrypt_data(const std::vector<uint8_t>& plaintext,
                     std::vector<uint8_t>& ciphertext);
    bool decrypt_data(const std::vector<uint8_t>& ciphertext,
                     std::vector<uint8_t>& plaintext);

    // ===== Helper Functions =====
    bool is_hid_device(libusb_device* device);
    std::string get_device_id(libusb_device* device);
    std::shared_ptr<USBDeviceDescriptor> build_device_descriptor(
        libusb_device* device, const std::string& device_id);
};

} // namespace NoteUSB

#endif // NOTE_USB_SESSION_H
