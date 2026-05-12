// include/note_usb/hid_streaming_thread.h
// HID streaming thread - reads from USB device and streams to client
// Simplified version without full HID parsing

#ifndef NOTE_USB_HID_STREAMING_THREAD_H
#define NOTE_USB_HID_STREAMING_THREAD_H

#include <thread>
#include <atomic>
#include <memory>
#include <libusb-1.0/libusb.h>
#include "notebytes.h"
#include "note_messaging.h"
#include "hid_constants.h"
#include "dro/spsc-queue.hpp"
#include "usb_device_descriptor.h"

// Forward declarations
namespace NoteUSB {
    class NoteUSBSession;
}

namespace NoteUSB {

/**
 * HID Report Event - represents a single HID report from the device
 */
struct HIDReportEvent {
    std::vector<uint8_t> data;
    uint64_t timestamp_ns;
    bool is_sentinel;

    HIDReportEvent() : timestamp_ns(0), is_sentinel(false) {}
    explicit HIDReportEvent(const uint8_t* d, int length)
        : data(d, d + length), timestamp_ns(0), is_sentinel(false) {}

    static HIDReportEvent sentinel() {
        HIDReportEvent e;
        e.is_sentinel = true;
        return e;
    }
};

/**
 * HID streaming thread - reads from USB device and sends events to client
 *
 * This is a simplified version that:
 * - Reads raw HID reports from the device
 * - Sends them to the client via socket
 * - Handles device disconnects gracefully
 */
class HIDStreamingThread {
public:
    /**
     * Create HID streaming thread
     * @param device Device descriptor with open handle
     * @param device_id Device ID for logging
     * @param client_fd Client socket file descriptor
     */
    HIDStreamingThread(std::shared_ptr<USBDeviceDescriptor> device,
                      const std::string& device_id,
                      int client_fd);
    
    /**
     * Set the parent session for encryption support
     * @param session Pointer to NoteUSBSession
     */
    void set_session(NoteUSBSession* session);

    ~HIDStreamingThread();

    /**
     * Start streaming thread
     */
    void start();

    /**
     * Stop streaming thread
     */

    /**
     * Notify device lost (called when device disconnects)
     */
    void notify_device_lost();

    /**
     * Stop streaming thread
     */
    void stop();

    /**
     * Check if thread is running
     */
    bool is_running() const;

    /**
     * Check if encryption is enabled
     */
    bool is_encryption_enabled() const;

    /**
     * Static callback for libusb transfer
     */
    static void LIBUSB_CALL transfer_callback(libusb_transfer* xfer);

private:
    // ===== State =====
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;

    libusb_context* usb_ctx_ = nullptr;
    libusb_context* ctx = nullptr;
    int client_fd_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    NoteUSBSession* session_ = nullptr;  // For encryption

    // ===== Threads =====
    std::thread capture_thread_;
    std::thread process_thread_;

    // ===== Transfer =====
    libusb_transfer* xfer_ = nullptr;

    // ===== Private Methods =====
    void capture_loop();
    void process_loop();
    void process_hid_report(const uint8_t* data, int length);
    void send_to_client(const std::vector<uint8_t>& data);

    // ===== Metrics =====
    std::atomic<uint64_t> events_read_{0};
    std::atomic<uint64_t> events_sent_{0};
    std::atomic<uint64_t> events_dropped_{0};

    // ===== Queue =====
    dro::SPSCQueue<HIDReportEvent> spsc_queue_{256};
};

} // namespace NoteUSB

#endif // NOTE_USB_HID_STREAMING_THREAD_H
