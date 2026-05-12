# One Device Per Connection - Architecture

## Overview

Implemented a **one device per connection** model where each NoteUSBSession is dedicated to exactly one client and one device.

## Key Design Decisions

### 1. Session = Device = Client
```
Before (Complex):
┌─────────────────────────────────────────┐
│      DeviceSession (Legacy)             │
│  - Manages multiple devices             │
│  - Streams to all connected clients     │
│  - Broadcasts hotplug to all sessions   │
│  - Complex routing logic                │
└─────────────────────────────────────────┘

After (Simplified):
┌─────────────────────────────────────────┐
│       NoteUSBSession (New)              │
│  - Manages ONE device only              │
│  - Streams to ONE client only           │
│  - Simple 1:1 mapping                   │
│  - Dedicated lifecycle                  │
└─────────────────────────────────────────┘
```

### 2. NoteDaemon = Pure Coordinator
```
NoteDaemon Core (Manager):
├── Accept connections
├── Get credentials (uid/gid/pid)
├── Route to module
└── NO hot path participation

NoteUSB Module:
├── handle_client(fd, pid) → Create session
├── Session reads messages
├── Session claims device
└── Session streams to client
```

---

## Architecture

### Component Flow

```
Client Connection
    ↓
NoteDaemon: accept() + get credentials
    ↓
NoteDaemon: route to module
    ↓
NoteUSBModule: handle_client(fd, pid)
    ↓
NoteUSBSession: created (discovery mode)
    ↓
NoteUSBSession: read_socket() loop
    ↓
Client Message → handle_message()
    ↓
    ├─ REQUEST_DISCOVERY → discover_devices()
    ├─ CLAIM_ITEM → claim_device(device_id)
    ├─ RELEASE_ITEM → release_device()
    └─ RESUME → handle_resume()
    ↓
Device claimed → Streaming starts
    ↓
HID events → Stream to client
    ↓
Client disconnects → cleanup_client(pid)
    ↓
NoteUSBSession: stopped, device released
```

---

## Files Created

### 1. NoteUSBSession Class

**Header:** `include/note_usb/note_usb_session.h`

**Implementation:** `src/note_usb_session.cpp`

**Key Features:**
```cpp
class NoteUSBSession {
public:
    // Create session
    NoteUSBSession(libusb_context* usb_ctx, int client_fd,
                   pid_t client_pid, const std::string& device_id = "");

    // Lifecycle
    void start();
    void stop();
    bool is_running() const;

    // Message handling
    void handle_message(const NoteBytes::Object& message);
    void read_socket();

private:
    // Device management (ONE device only)
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;

    // Streaming
    std::unique_ptr<std::thread> streaming_thread_;

    // Message handlers
    void handle_discover(...);
    void handle_claim(...);
    void handle_release(...);
    void handle_resume(...);

    // Device operations
    NoteDaemon::Error discover_devices();
    NoteDaemon::Error claim_device(const std::string& device_id);
    NoteDaemon::Error release_device();
};
```

---

## Session Lifecycle

### 1. Creation

```cpp
// In NoteUSBModule::handle_client()
Error handle_client(int client_fd, pid_t client_pid) {
    // Check if session exists
    if (sessions_.count(client_pid) > 0) {
        return Error(ALREADY_INITIALIZED, "Session exists");
    }

    // Create session (discovery only)
    auto session = std::make_unique<NoteUSBSession>(
        usb_ctx_, client_fd, client_pid);

    // Start reading from socket
    session->start();

    // Store session
    sessions_[client_pid] = std::move(session);

    return Error(SUCCESS, "");
}
```

### 2. Message Handling

```cpp
void read_socket() {
    for (;;) {
        // Read message type
        auto event_val = reader.read_value();
        std::string event_type = event_val->as_string();

        // Read message body
        auto event_obj = NoteBytes::Object::deserialize(...);

        // Route to handler
        handle_message(event_obj);
    }
}
```

### 3. Device Claiming

```cpp
void handle_claim(const NoteBytes::Object& message) {
    std::string device_id = message.get_string("device_id");

    // Claim the device
    auto err = claim_device(device_id);
    if (err.failed()) {
        send_error(...);
        return;
    }

    // Send success response
    send_response(success_response);
}
```

### 4. Cleanup

```cpp
// In NoteUSBModule::cleanup_client()
void cleanup_client(pid_t client_pid) {
    auto it = sessions_.find(client_pid);
    if (it != sessions_.end()) {
        // Stop session (releases device)
        it->second->stop();

        // Remove from map
        sessions_.erase(it);
    }
}
```

---

## Message Flow

### Request Discovery

```
Client → NoteDaemon
    ↓
NoteDaemon → NoteUSBModule (handle_client)
    ↓
NoteUSBSession → handle_discover()
    ↓
NoteUSBSession → discover_devices()
    ↓
NoteUSBSession → send_response(ITEM_LIST)
    ↓
Client receives device list
```

### Claim Device

```
Client → NoteDaemon
    ↓
NoteDaemon → NoteUSBModule (handle_client)
    ↓
NoteUSBSession → handle_claim(device_id)
    ↓
NoteUSBSession → claim_device(device_id)
    ↓
    ├─ Find device in libusb
    ├─ libusb_open()
    ├─ libusb_claim_interface()
    ├─ libusb_detach_kernel_driver()
    └─ Build device descriptor
    ↓
NoteUSBSession → send_response(ITEM_CLAIMED)
    ↓
Client receives success
```

### Release Device

```
Client → NoteDaemon
    ↓
NoteDaemon → NoteUSBModule (handle_client)
    ↓
NoteUSBSession → handle_release()
    ↓
NoteUSBSession → release_device()
    ↓
    ├─ libusb_release_interface()
    ├─ libusb_attach_kernel_driver()
    └─ libusb_close()
    ↓
NoteUSBSession → send_response(ITEM_RELEASED)
    ↓
Client receives success
```

---

## Integration Points

### IModule Interface Updates

**File:** `include/module_framework/imodule.h`

```cpp
class IModule {
public:
    // ... existing methods ...

    // NEW: Handle a new client connection
    virtual Error handle_client(int client_fd, pid_t client_pid) = 0;

    // NEW: Cleanup client session
    virtual void cleanup_client(pid_t client_pid) = 0;
};
```

### NoteUSBModule Implementation

**File:** `src/module.cpp`

```cpp
class NoteUSBModule : public NoteDaemon::IModule {
private:
    libusb_context* usb_ctx_ = nullptr;
    std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;

public:
    // Create session for client
    Error handle_client(int client_fd, pid_t client_pid) override {
        // Check if session exists
        if (sessions_.count(client_pid) > 0) {
            return Error(ALREADY_INITIALIZED, "Session exists");
        }

        // Create session
        auto session = std::make_unique<NoteUSBSession>(
            usb_ctx_, client_fd, client_pid);
        session->start();

        // Store session
        sessions_[client_pid] = std::move(session);

        return Error(SUCCESS, "");
    }

    // Cleanup client session
    void cleanup_client(pid_t client_pid) override {
        auto it = sessions_.find(client_pid);
        if (it != sessions_.end()) {
            it->second->stop();
            sessions_.erase(it);
        }
    }
};
```

### NoteDaemon Main Updates

**File:** `src/main.cpp`

```cpp
void handle_client_modular(int client_fd, pid_t client_pid) {
    auto* note_usb_module = module_registry_.get_module("note_usb");
    if (note_usb_module) {
        // Create session
        Error err = note_usb_module->handle_client(client_fd, client_pid);
        if (err.failed()) {
            syslog(LOG_ERR, "Failed to handle client: %s", err.message().data());
            safe_close(client_fd);
        }
    }
}

// In message loop, after client disconnects:
note_usb_module->cleanup_client(client_pid);
```

---

## Benefits

### 1. Simplicity
- **Clear 1:1 mapping:** One session = one device = one client
- **No complex routing:** No need for device-level routing
- **No hotplug broadcasting:** Each session discovers devices independently

### 2. Scalability
- **Independent sessions:** Sessions don't interfere with each other
- **Isolated state:** Each session has its own device handle
- **Easy to add:** New sessions just add to the map

### 3. Maintainability
- **Clear lifecycle:** Session created on connect, destroyed on disconnect
- **Simple error handling:** Errors are contained within the session
- **Easy to debug:** One session = one thread = one device

### 4. Performance
- **No contention:** Each session has its own resources
- **No locks needed:** Session state is not shared
- **Efficient streaming:** Direct socket write per session

---

## State Management

### Session State

```cpp
class NoteUSBSession {
private:
    // Socket state
    int client_fd_;
    pid_t client_pid_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;

    // Device state (ONE device only)
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;

    // Streaming state
    std::unique_ptr<std::thread> streaming_thread_;

    // Metrics
    std::atomic<int> events_dropped_;
    std::atomic<int> events_queued_;
    std::atomic<int> events_delivered_;
};
```

### Module State

```cpp
class NoteUSBModule : public NoteDaemon::IModule {
private:
    libusb_context* usb_ctx_;
    std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;

    // Other components...
    std::unique_ptr<DeviceHandler> device_handler_;
    std::unique_ptr<NoteDaemon::HandlerRegistry> handler_registry_;
};
```

---

## Thread Safety

### Session-Level Thread Safety

```cpp
// Atomic flags for coordination
std::atomic<bool> running_{false};
std::atomic<bool> stop_requested_{false};

// Atomic metrics
std::atomic<int> events_dropped_{0};
std::atomic<int> events_queued_{0};
std::atomic<int> events_delivered_{0};
```

### Module-Level Thread Safety

```cpp
// Sessions map is protected by module's mutex (if needed)
std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;

// DeviceHandler has its own mutex
class DeviceHandler {
    std::mutex mutex_;
    // ...
};
```

---

## Error Handling

### Per-Session Errors

```cpp
void handle_claim(const NoteBytes::Object& message) {
    std::string device_id = message.get_string("device_id");

    auto err = claim_device(device_id);
    if (err.failed()) {
        send_error(
            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
            device_id,
            err.code,
            err.message(),
            correlation_id
        );
        return;  // Session continues running
    }

    send_response(success_response);
}
```

### Session-Level Error Recovery

```cpp
void read_socket() {
    for (;;) {
        try {
            // Read and process message
            auto event_obj = NoteBytes::Object::deserialize(...);
            handle_message(event_obj);
        } catch (const std::exception& e) {
            syslog(LOG_ERR, "Error in message loop: %s", e.what());
            break;  // Exit loop, session will be cleaned up
        }

        if (stop_requested_.load()) {
            break;
        }
    }
}
```

---

## Future Enhancements

### 1. Device Pooling (Optional)
If multiple clients need to access the same device:
```cpp
// Share device handle among sessions
std::shared_ptr<USBDeviceDescriptor> device_;

// Use reference counting to track active sessions
std::atomic<int> session_count_{0};
```

### 2. Session Timeout (Optional)
```cpp
// Auto-cleanup idle sessions
std::chrono::steady_clock::time_point last_activity_;

void check_timeout() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
        now - last_activity_);

    if (elapsed > std::chrono::minutes(30)) {
        stop();
        cleanup_client(client_pid_);
    }
}
```

### 3. Device Reconnection (Optional)
```cpp
// If device disconnects, keep session alive
void handle_device_disconnect() {
    // Don't close session
    // Wait for device to reattach
    // Re-claim device when available
}
```

---

## Testing Strategy

### Unit Tests

```cpp
TEST(NoteUSBSession, CreateDestroy) {
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    NoteUSBSession session(ctx, 1, 100, "");
    ASSERT_TRUE(session.is_running());

    session.stop();
    ASSERT_FALSE(session.is_running());

    libusb_exit(ctx);
}

TEST(NoteUSBSession, DiscoverDevices) {
    NoteUSBSession session(ctx, 1, 100, "");

    auto err = session.discover_devices();
    ASSERT_TRUE(err.success());

    ASSERT_TRUE(session.device_ != nullptr);
}

TEST(NoteUSBSession, ClaimDevice) {
    NoteUSBSession session(ctx, 1, 100, "1:2");

    auto err = session.claim_device("1:2");
    ASSERT_TRUE(err.success());

    ASSERT_TRUE(session.device_->handle != nullptr);
}
```

### Integration Tests

```bash
# Test session lifecycle
1. Start daemon
2. Connect client
3. Verify session created
4. Send REQUEST_DISCOVERY
5. Verify device list received
6. Send CLAIM_ITEM
7. Verify device claimed
8. Send RELEASE_ITEM
9. Verify device released
10. Disconnect client
11. Verify session cleaned up

# Test concurrent sessions
1. Connect client 1
2. Connect client 2
3. Verify both sessions active
4. Claim different devices
5. Verify each session has correct device
```

---

## Summary

| Aspect | Before | After |
|--------|--------|-------|
| Session-to-Device | 1:N (many sessions, one device) | 1:1 (one session, one device) |
| Session-to-Client | 1:N (one session, many clients) | 1:1 (one session, one client) |
| NoteDaemon Role | Partial (some hot path) | Pure coordinator |
| Hotplug | Broadcast to all sessions | Discovery per session |
| Complexity | High (complex routing) | Low (simple mapping) |
| Scalability | Limited (shared state) | High (isolated sessions) |

This architecture provides a **clean, simple, and scalable** model for USB device handling in the NoteUSB module.
