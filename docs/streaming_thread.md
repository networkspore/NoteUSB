// include/note_usb/hid_streaming_thread.h
// HID streaming thread implementation

#include "note_usb/hid_streaming_thread.h"
#include "note_usb/note_usb_session.h"
#include <syslog.h>
#include <unistd.h>
#include <chrono>
#include <cstring>

using namespace NoteUSB;

// =============================================================================
// HIDStreamingThread implementation
// =============================================================================

HIDStreamingThread::HIDStreamingThread(std::shared_ptr<USBDeviceDescriptor> device,
                                      const std::string& device_id,
                                      int client_fd)
    : device_(device), device_id_(device_id), client_fd_(client_fd) {

    syslog(LOG_INFO, "HIDStreamingThread: created for device %s", device_id.c_str());
}

HIDStreamingThread::~HIDStreamingThread() {
    stop();
    syslog(LOG_INFO, "HIDStreamingThread: destroyed for device %s", device_id_.c_str());
}

void HIDStreamingThread::start() {
    if (running_.load(std::memory_order_relaxed)) {
        return;
    }

    running_.store(true, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);

    // Allocate transfer
    xfer_ = libusb_alloc_transfer(0);
    if (!xfer_) {
        syslog(LOG_ERR, "HIDStreamingThread: failed to allocate libusb transfer for %s",
               device_id_.c_str());
        running_.store(false, std::memory_order_release);
        return;
    }

    // Allocate buffer for transfer
    uint8_t* buffer = new (std::nothrow) uint8_t[64];
    if (!buffer) {
        syslog(LOG_ERR, "HIDStreamingThread: failed to allocate buffer for %s",
               device_id_.c_str());
        libusb_free_transfer(xfer_);
        xfer_ = nullptr;
        running_.store(false, std::memory_order_release);
        return;
    }

    // Fill interrupt transfer
    libusb_fill_interrupt_transfer(
        xfer_,
        device_->handle,
        HidConstants::kDefaultEndpointIn,
        buffer,
        HidConstants::kHidReportBufferSize,
        &HIDStreamingThread::transfer_callback,
        this,
        0  // no timeout - handled by event loop
    );

    int rc = libusb_submit_transfer(xfer_);
    if (rc != LIBUSB_SUCCESS) {
        syslog(LOG_ERR, "HIDStreamingThread: failed to submit transfer for %s: %s",
               device_id_.c_str(), libusb_error_name(rc));
        delete[] buffer;
        libusb_free_transfer(xfer_);
        xfer_ = nullptr;
        running_.store(false, std::memory_order_release);
        return;
    }

    // Start threads
    capture_thread_ = std::thread(&HIDStreamingThread::capture_loop, this);
    process_thread_ = std::thread(&HIDStreamingThread::process_loop, this);

    syslog(LOG_INFO, "HIDStreamingThread: started for device %s", device_id_.c_str());
}

void HIDStreamingThread::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);

    // Signal process thread to exit
    (void)spsc_queue_.try_push(HIDReportEvent::sentinel());

    // Cancel transfer (will make capture loop exit)
    if (xfer_) {
        libusb_cancel_transfer(xfer_);
    }

    // Join threads
    if (capture_thread_.joinable()) capture_thread_.join();
    if (process_thread_.joinable()) process_thread_.join();

    // Free transfer structure
    if (xfer_) {
        libusb_free_transfer(xfer_);
        xfer_ = nullptr;
    }

    syslog(LOG_INFO, "HIDStreamingThread: stopped for device %s", device_id_.c_str());
}

bool HIDStreamingThread::is_running() const {
    return running_.load(std::memory_order_relaxed);
}

// =============================================================================
// Callback
// =============================================================================

void LIBUSB_CALL HIDStreamingThread::transfer_callback(libusb_transfer* xfer) {
    auto* self = static_cast<HIDStreamingThread*>(xfer->user_data);

    // Guard: if running_ is false, the object may be in destruction
    if (!self->running_.load(std::memory_order_relaxed)) {
        return;
    }

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length > 0) {
        // Push event to queue (lock-free, drop if full)
        HIDReportEvent event(xfer->buffer, xfer->actual_length);
        event.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (!self->spsc_queue_.try_push(event)) {
            // Queue full, drop event
            self->events_dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            self->events_read_.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (xfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        // Device disconnected - notify via queue
        self->notify_device_lost();
    } else {
        // Resubmit if still running and not in a terminal state
        if (self->running_.load(std::memory_order_relaxed)) {
            int rc = libusb_submit_transfer(xfer);
            if (rc != LIBUSB_SUCCESS) {
                syslog(LOG_ERR, "HIDStreamingThread: failed to resubmit transfer for %s: %s",
                       self->device_id_.c_str(), libusb_error_name(rc));
                self->running_.store(false, std::memory_order_release);
            }
        }
    }
}

void HIDStreamingThread::notify_device_lost() {
    // Guard: use exchange to prevent double-notification
    if (!running_.exchange(false, std::memory_order_release)) return;

    // Signal the process thread to exit
    (void)spsc_queue_.try_push(HIDReportEvent::sentinel());

    syslog(LOG_WARNING, "HIDStreamingThread: device %s disconnected",
           device_id_.c_str());
}

// =============================================================================
// Capture Thread
// =============================================================================

void HIDStreamingThread::capture_loop() {
    // This thread runs the libusb event loop
    libusb_context* ctx = device_ ? nullptr : nullptr;

    struct timeval tv = {0, HidConstants::kLibusbPollTimeoutUs};

    while (running_.load(std::memory_order_relaxed)) {
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }

    // Cleanup: wait for cancelled transfer to complete
    struct timeval tv2 = {0, 100000};
    libusb_handle_events_timeout_completed(ctx, &tv2, nullptr);

    syslog(LOG_DEBUG, "HIDStreamingThread: capture loop exited for device %s",
           device_id_.c_str());
}

// =============================================================================
// Process Thread
// =============================================================================

void HIDStreamingThread::process_loop() {
    HIDReportEvent event;

    while (true) {
        if (spsc_queue_.try_pop(event)) {
            if (event.is_sentinel) break;

            process_hid_report(event.data.data(), (int)event.data.size());
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Drain remaining events
    while (spsc_queue_.try_pop(event)) {
        if (!event.is_sentinel && !event.data.empty()) {
            process_hid_report(event.data.data(), (int)event.data.size());
        }
    }

    syslog(LOG_DEBUG, "HIDStreamingThread: process loop exited for device %s",
           device_id_.c_str());
}

// =============================================================================
// Processing
// =============================================================================

void HIDStreamingThread::process_hid_report(const uint8_t* data, int length) {
    // Create event packet
    NoteBytes::Object event;
    event.add(NoteMessaging::Keys::DEVICE_ID, device_id_);

    // Use raw bytes as payload
    NoteBytes::Array payload;
    for (int i = 0; i < length; i++) {
        payload.add(NoteBytes::Value(data[i]));
    }
    event.add(NoteMessaging::Keys::PAYLOAD, payload.as_value());

    // Serialize and send
    std::vector<uint8_t> packet = event.serialize_with_header();
    send_to_client(packet);

    events_sent_.fetch_add(1, std::memory_order_relaxed);
}

void HIDStreamingThread::send_to_client(const std::vector<uint8_t>& data) {
    // Write to socket
    ssize_t sent = write(client_fd_, data.data(), data.size());
    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Would block - this shouldn't happen with proper socket setup
            syslog(LOG_WARNING, "HIDStreamingThread: send would block for device %s",
                   device_id_.c_str());
        } else {
            // Error - device likely disconnected
            syslog(LOG_ERR, "HIDStreamingThread: failed to send to client for device %s: %s",
                   device_id_.c_str(), strerror(errno));
            running_.store(false, std::memory_order_release);
        }
    }
}

// =============================================================================
// Metrics
// =============================================================================

uint64_t HIDStreamingThread::get_events_read() const {
    return events_read_.load(std::memory_order_relaxed);
}

uint64_t HIDStreamingThread::get_events_sent() const {
    return events_sent_.load(std::memory_order_relaxed);
}

uint64_t HIDStreamingThread::get_events_dropped() const {
    return events_dropped_.load(std::memory_order_relaxed);
}

} // namespace NoteUSB
