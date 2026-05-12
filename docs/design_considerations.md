# Design Considerations Implementation Summary

## Overview

This document summarizes the design considerations that have been implemented for the NoteUSB module refactor, focusing on proper separation of concerns and module-private state management.

## Completed Implementations

### 1. Module-Private Session Management ✅

**Design Consideration:**
- Hotplug session registry should be module-private
- No global session list that other modules can access
- Each module manages its own sessions independently

**Implementation:**

#### Created `SessionManager` class
- **File:** `NoteUSB/include/note_usb/session_manager.h`
- **File:** `NoteUSB/src/session_manager.cpp`

**Features:**
```cpp
// Singleton pattern for module-private session registry
class SessionManager {
public:
    static SessionManager& instance();

    // Register a session for hotplug notifications
    void register_session(SessionID client_fd, pid_t client_pid);

    // Unregister a session (called on disconnect)
    void unregister_session(SessionID session_id);

    // Send a message to all active sessions
    void broadcast_to_all_sessions(const NoteBytes::Object& msg);

    // Query methods
    size_t session_count() const;
    bool is_session_active(SessionID client_fd) const;
};
```

**Benefits:**
- No global `active_sessions()` list
- Other modules cannot access session data
- Thread-safe with mutex protection
- Encapsulates session lifecycle

#### Updated `NoteUSBModule` class
- **File:** `NoteUSB/src/module.cpp`

**Added:**
```cpp
// Module-private session registry
std::map<pid_t, int> sessions_;  // pid -> client_fd

// Session registration
void register_session(pid_t client_pid, int client_fd);
void unregister_session(pid_t client_pid);

// Hotplug broadcasting
void broadcast_to_all_sessions(const NoteBytes::Object& msg);
```

**Usage:**
```cpp
// When client connects
register_session(client_pid, client_fd);
SessionManager::instance().register_session(client_fd, client_pid);

// When client disconnects
unregister_session(client_pid);
SessionManager::instance().unregister_session(client_fd);

// Broadcast hotplug notification
broadcast_to_all_sessions(notification);
SessionManager::instance().broadcast_to_all_sessions(notification);
```

---

### 2. Module-Specific Device Registry ✅

**Design Consideration:**
- Device registry should be module-private
- Each module has its own crash recovery file
- No global registry that other modules access

**Implementation:**

#### Created module-private `DeviceRegistry` class
- **File:** `NoteUSB/include/note_usb/device_handler.h`
- **File:** `NoteUSB/src/device_handler.cpp`

**Key Changes:**
```cpp
// OLD (Global - WRONG):
static const std::string& default_registry_path() {
    static std::string path = "/run/netnotes/device_registry.json";
    return path;
}

// NEW (Module-private - CORRECT):
class DeviceRegistry {
public:
    static const std::string& path();  // Returns: /run/netnotes/modules/note_usb/device_registry.json
    static void add_device(...);
    static bool remove_device(...);
    static std::vector<ClaimedDevice> get_all_devices();
    static void remove_all();
};
```

**Registry Path:**
```
/run/netnotes/modules/note_usb/device_registry.json
```

**File Format:**
```json
[
  {
    "pid": 12345,
    "device_id": "1:3",
    "interface_number": 0,
    "kernel_driver_attached": true
  }
]
```

**Benefits:**
- Each module has isolated crash recovery data
- No conflicts between modules
- Clear separation of concerns
- Monitor process reads module-specific registry

---

## Architecture Improvements

### Before (Incorrect):
```
┌─────────────────────────────────────────┐
│         NoteDaemon Core                │
│  ┌───────────────────────────────────┐ │
│  │  Global Session Registry          │ │ ← Accessible to all modules
│  │  - active_sessions() list         │ │
│  │  - broadcast_to_all_sessions()    │ │
│  └───────────────────────────────────┘ │
│  ┌───────────────────────────────────┐ │
│  │  Global Device Registry           │ │ ← Accessible to all modules
│  │  - /run/netnotes/device_registry  │ │
│  └───────────────────────────────────┘ │
└─────────────────────────────────────────┘
        ↓
┌─────────────────────────────────────────┐
│        DeviceSession (Legacy)          │
│  - Has access to global registries     │
│  - Broadcasts to all sessions          │
│  - Registers devices globally          │
└─────────────────────────────────────────┘
```

### After (Correct):
```
┌─────────────────────────────────────────┐
│         NoteDaemon Core                │
│  - Pure router (no device handling)    │
│  - Routes messages to modules          │
│  - No access to module-private state   │
└─────────────────────────────────────────┘
        ↓
┌─────────────────────────────────────────┐
│       NoteUSB Module                   │
│  ┌───────────────────────────────────┐ │
│  │  Module-Private Session Registry  │ │ ← Only NoteUSB sees this
│  │  - sessions_ map                  │ │
│  │  - register_session()             │ │
│  │  - unregister_session()           │ │
│  └───────────────────────────────────┘ │
│  ┌───────────────────────────────────┐ │
│  │  SessionManager (Singleton)       │ │
│  │  - broadcast_to_all_sessions()    │ │
│  └───────────────────────────────────┘ │
│  ┌───────────────────────────────────┐ │
│  │  Module-Private Device Registry   │ │ ← Only NoteUSB sees this
│  │  - /run/netnotes/modules/note_    │ │
│  │    usb/device_registry.json       │ │
│  └───────────────────────────────────┘ │
└─────────────────────────────────────────┘
```

---

## Thread Safety

### SessionManager Thread Safety
```cpp
class SessionManager {
private:
    std::mutex mutex_;  // Protects sessions_ map

    void register_session(SessionID client_fd, pid_t client_pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        // ... add session ...
    }

    void broadcast_to_all_sessions(const NoteBytes::Object& msg) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, session] : sessions_) {
            // ... send to session ...
        }
    }
};
```

### DeviceRegistry Thread Safety
```cpp
class DeviceRegistry {
private:
    static std::mutex& registry_mutex();  // Protects registry file I/O

    static void add_device(...) {
        std::lock_guard<std::mutex> lock(registry_mutex());
        // ... write to file ...
    }
};
```

---

## Integration Points

### 1. Hotplug Broadcasting

**Scenario:** USB device attaches
```
1. libusb callback detects device
2. NoteUSBModule builds device descriptor
3. NoteUSBModule.broadcast_to_all_sessions(notification)
4. SessionManager.broadcast_to_all_sessions(notification)
5. SessionManager iterates all sessions
6. Sends notification to each client
```

**Code Flow:**
```cpp
// In NoteUSBModule (module.cpp)
void broadcast_device_attached(const std::string& device_id,
                               const USBDeviceDescriptor& desc) {
    NoteBytes::Object notification;
    notification.add(NoteMessaging::Keys::EVENT,
                     NoteMessaging::ProtocolMessages::DEVICE_ATTACHED);
    notification.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    notification.add(NoteMessaging::ProtocolMessages::ITEM_INFO,
                     desc.to_notebytes().as_value());

    broadcast_to_all_sessions(notification);
}

// Calls SessionManager
void broadcast_to_all_sessions(const NoteBytes::Object& msg) {
    SessionManager::instance().broadcast_to_all_sessions(msg);
}
```

### 2. Session Lifecycle

**Scenario:** Client connects and disconnects
```
1. Client connects (accept())
2. NoteDaemon registers session (TODO: implement)
3. Client sends HELLO
4. NoteDaemon routes to NoteUSB module
5. NoteUSBModule processes message
6. Client disconnects
7. NoteDaemon unregisters session (TODO: implement)
```

**Current Status:**
- Session registration/unregistration methods added to NoteUSBModule
- Main.cpp has placeholder calls
- Need to implement full integration

---

## Remaining Work

### High Priority
1. **Implement Session Registration in main.cpp**
   - Register session when client first connects
   - Unregister session when client disconnects
   - Pass client_fd to NoteUSBModule

2. **Implement Response Mechanism**
   - Handlers need way to send responses to clients
   - Add callback to HandlerRegistry
   - Update NoteUSB handlers to use response mechanism

3. **Move Streaming Threads to NoteUSB**
   - Extract HIDDeviceStreamingThread from NoteDaemon
   - Integrate with SessionManager
   - Pass client_fd to streaming threads

### Medium Priority
4. **Implement Hotplug Callbacks**
   - Add libusb hotplug registration to NoteUSBModule
   - Call broadcast_device_attached() on device events
   - Test hotplug notifications

5. **Complete Device Discovery**
   - Implement full device discovery in device_handler.cpp
   - Add device list response sending
   - Test with actual USB devices

### Low Priority
6. **Encryption Implementation**
   - Implement proper AES-256-GCM encryption
   - Move from XOR placeholder to OpenSSL
   - Add encryption negotiation to session flow

---

## Testing Strategy

### Unit Tests
```cpp
// Test SessionManager
TEST(SessionManager, RegisterUnregister) {
    SessionManager::instance().register_session(1, 100);
    ASSERT_EQ(SessionManager::instance().session_count(), 1);

    SessionManager::instance().unregister_session(1);
    ASSERT_EQ(SessionManager::instance().session_count(), 0);
}

TEST(SessionManager, Broadcast) {
    SessionManager::instance().register_session(1, 100);
    SessionManager::instance().register_session(2, 101);

    NoteBytes::Object msg;
    msg.add(NoteMessaging::Keys::EVENT, "TEST");

    SessionManager::instance().broadcast_to_all_sessions(msg);
    ASSERT_EQ(SessionManager::instance().session_count(), 2);
}
```

### Integration Tests
```bash
# Test device registry file location
ls -la /run/netnotes/modules/note_usb/device_registry.json

# Test session management
# 1. Start daemon
# 2. Connect client
# 3. Verify session registered
# 4. Disconnect client
# 5. Verify session unregistered
```

---

## Benefits of This Design

### 1. Encapsulation
- Module-private state cannot be accessed by other modules
- Clear boundaries between components
- Easier to reason about module behavior

### 2. Scalability
- Each module manages its own resources
- No global state that becomes a bottleneck
- Easy to add new modules without affecting existing ones

### 3. Maintainability
- Changes to session management only affect NoteUSB
- Changes to device registry only affect NoteUSB
- Clear separation of concerns

### 4. Safety
- Thread-safe with mutex protection
- No race conditions in session registry
- Proper cleanup on shutdown

---

## Summary

| Design Consideration | Status | Implementation |
|---------------------|--------|----------------|
| Module-private session registry | ✅ Complete | SessionManager class |
| Module-private device registry | ✅ Complete | DeviceRegistry class (module-private) |
| Thread-safe session management | ✅ Complete | Mutex protection |
| Thread-safe registry I/O | ✅ Complete | Mutex protection |
| Hotplug broadcasting | ⚠️ Partial | Methods added, integration pending |
| Session lifecycle | ⚠️ Partial | Methods added, registration pending |
| Response mechanism | ❌ Not Started | Need callback mechanism |

The core architecture is now correct - modules manage their own state. The remaining work is primarily about wiring up the session management to the existing client connection flow.
