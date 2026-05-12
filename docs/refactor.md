# NoteUSB Refactor - Missing Features

This document lists all features that are currently stubs or missing in the NoteUSB module compared to the original NoteDaemon device handling code.

## Overview

The NoteUSB module is being built as a refactored, modular version of the original NoteDaemon USB device handling. Some features are fully implemented, while others are stubs that need to be completed.

---

## Complete Features

### 1. Module Framework (NoteDaemon Core)
- IModule interface
- Module loading/discovery
- Handler registry
- Config loading
- Error collection

**Location:** `NoteDaemon/include/module_framework/`

### 2. Device Monitor
Fully implemented - reattaches kernel drivers on daemon exit.

**Location:** `NoteUSB/monitor/note_usb_monitor.cpp`

**Original code:** `NoteDaemon/process-monitor/src/process_monitor.cpp`

**Implementation:**
- Polls for daemon PID exit
- Reads DeviceRegistry for claimed devices
- Reattaches kernel drivers via libusb
- Cleans up registry entries

---

## Stub/Missing Features

### 1. Device Discovery

**Status:** Stub

**Current implementation (`device_handler.cpp`):**
```cpp
void DeviceHandler::start_discovery() {
    if (running_) return;
    running_ = true;
    syslog(LOG_INFO, "NoteUSB: device discovery started");
}
```

**What it should do:**
- Scan USB bus for HID devices
- Build device descriptors (bus:address, VID:PID, interface class)
- Store in available devices map

**Original code reference:**
- `NoteDaemon/src/device_session.cpp` - `discover_devices()` method (lines ~580-660)
- `NoteDaemon/include/device_session.h` - `USBDeviceDescriptor` class
- `NoteDaemon/include/usb_device_descriptor.h` - Device descriptor structure

**Key functions to reference:**
```cpp
// From device_session.cpp
void DeviceSession::discover_devices() {
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(usb_ctx_, &device_list);
    // ... iterate and check for HID devices
    libusb_free_device_list(device_list, 1);
}
```

---

### 2. Device Claim

**Status:** Stub

**Current implementation:** Just logs, doesn't actually claim device

**What it should do:**
1. Parse device_id from message
2. Find device in available devices
3. Open device with libusb_open()
4. Claim interface with libusb_claim_interface()
5. Detach kernel driver if active
6. Register with DeviceRegistry for crash recovery
7. Create streaming thread
8. Send success response to client

**Original code reference:**
- `NoteDaemon/src/device_session.cpp` - `handle_claim_device()` method (lines ~700-800)
- `NoteDaemon/include/device_session.h` - Lines ~450-550

**Key implementation:**
```cpp
// From device_session.cpp
void DeviceSession::handle_claim_device(const NoteBytes::Object& msg) {
    std::string device_id = msg.get_string(NoteMessaging::Keys::DEVICE_ID, ...);
    
    // Open device
    libusb_device_handle* handle = nullptr;
    int result = libusb_open(usb_device, &handle);
    
    // Claim interface
    result = libusb_claim_interface(handle, device_desc->interface_number);
    
    // Detach kernel driver
    if (libusb_kernel_driver_active(handle, ...) == 1) {
        libusb_detach_kernel_driver(handle, ...);
    }
    
    // Register for crash recovery
    this->register_claimed_device(device_id, interface_number, kernel_driver_attached);
}
```

---

### 3. Device Release

**Status:** Stub

**Current implementation:** Just logs, doesn't actually release device

**What it should do:**
1. Parse device_id from message
2. Verify caller owns the device
3. Stop streaming thread
4. Release interface with libusb_release_interface()
5. Reattach kernel driver if it was detached
6. Close device handle
7. Unregister from DeviceRegistry
8. Send success response

**Original code reference:**
- `NoteDaemon/src/device_session.cpp` - `handle_release_device()` method (lines ~820-920)

**Key implementation:**
```cpp
// From device_session.cpp
void DeviceSession::handle_release_device(const NoteBytes::Object& msg) {
    // Stop streaming thread
    streaming_threads[device_id]->stop();
    
    // Release interface
    libusb_release_interface(handle, interface_number);
    
    // Reattach kernel driver
    if (kernel_driver_attached) {
        libusb_attach_kernel_driver(handle, interface_number);
    }
    
    // Close handle
    libusb_close(handle);
    
    // Unregister from crash recovery
    this->unregister_device(device_id);
}
```

---

### 4. Device List Response (Discovery)

**Status:** Stub

**Current implementation:** Handler just logs, doesn't send response

**What it should do:**
- Iterate through available devices
- Serialize to NoteBytes::Array
- Send as ITEM_LIST response

**Original code reference:**
- `NoteDaemon/src/device_session.cpp` - `send_device_list()` method

**Key implementation:**
```cpp
// From device_session.cpp
void DeviceSession::send_device_list() {
    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT, EventBytes::TYPE_CMD);
    response.add(NoteMessaging::Keys::CMD, NoteMessaging::ProtocolMessages::ITEM_LIST);
    
    NoteBytes::Array devices_array;
    for (const auto& [id, device] : available_devices) {
        auto device_obj = device->to_notebytes();
        devices_array.add(NoteBytes::Value(device_obj.serialize(), NoteBytes::Type::OBJECT));
    }
    response.add(NoteMessaging::Keys::ITEMS, devices_array.as_value());
    
    send_message(response);
}
```

---

### 5. Device Registry Integration

**Status:** Partial

**Current implementation:** Monitor uses DeviceRegistry, but device_handler doesn't register devices

**What it should do:**
When claiming a device:
```cpp
void register_claimed_device(const std::string& device_id, int interface_number, bool kernel_driver_attached) {
    ClaimedDevice device;
    device.pid = getpid();  // Daemon's PID
    device.device_id = device_id;
    device.interface_number = interface_number;
    device.kernel_driver_attached = kernel_driver_attached;
    DeviceRegistry::add_device(device);
}
```

When releasing a device:
```cpp
void unregister_device(const std::string& device_id) {
    DeviceRegistry::remove_device(getpid(), device_id);
}
```

**Original code reference:**
- `NoteDaemon/src/device_session.cpp` - Lines ~350-380 (register_claimed_device, unregister_device)
- `NoteDaemon/src/device_registry.cpp` - DeviceRegistry implementation

---

### 6. Encryption

**Status:** Placeholder (XOR cipher)

**Current implementation:** Simple XOR encryption, not production-ready

**What it should do:**
- Use AES-256-GCM for authenticated encryption
- Proper key derivation
- IV/nonce handling

**Original code reference:**
- `NoteDaemon/include/encryption_protocol.h` - Encryption protocol design
- `NoteDaemon/src/core/encryption_api.cpp` - Current placeholder implementation

**Note:** The current implementation in `encryption_api.cpp` is a placeholder using XOR. This should be replaced with proper AES-256-GCM using OpenSSL.

---

### 7. Streaming Thread

**Status:** Not yet moved to NoteUSB

**Current implementation:** Still in NoteDaemon as HIDDeviceStreamingThread

**What it should do:**
- Read from USB endpoint in a loop
- Parse HID reports
- Send to client via NoteBytes messages
- Handle flow control (backpressure)

**Original code reference:**
- `NoteDaemon/include/hid_device_streaming_thread.h` - Streaming thread interface
- `NoteDaemon/src/hid_device_streaming_thread.cpp` - HID specific implementation

---

## Feature Implementation Priority

| Priority | Feature | Files to Modify |
|----------|---------|-----------------|
| High | Device Discovery | `device_handler.cpp`, `device_discovery.cpp` |
| High | Device Claim | `device_handler.cpp` |
| High | Device Release | `device_handler.cpp` |
| High | Device List Response | `device_handler.cpp` |
| Medium | Device Registry Integration | `device_handler.cpp` |
| Low | Encryption | `encryption_api.cpp` |
| Low | Streaming Thread | New file in `src/streaming_thread.cpp` |

---

## References

### Original Code Locations

| Feature | File | Lines |
|---------|------|-------|
| Device Discovery | `NoteDaemon/src/device_session.cpp` | ~580-660 |
| Device Claim | `NoteDaemon/src/device_session.cpp` | ~700-800 |
| Device Release | `NoteDaemon/src/device_session.cpp` | ~820-920 |
| Device List | `NoteDaemon/src/device_session.cpp` | ~600-615 |
| Registry Registration | `NoteDaemon/src/device_session.cpp` | ~350-380 |
| Device Registry | `NoteDaemon/src/device_registry.cpp` | All |
| Streaming Thread | `NoteDaemon/src/hid_device_streaming_thread.cpp` | All |
| Encryption | `NoteDaemon/src/core/encryption_api.cpp` | All (placeholder) |

### Related Headers

- `NoteDaemon/include/device_session.h`
- `NoteDaemon/include/device_registry.h`
- `NoteDaemon/include/usb_device_descriptor.h`
- `NoteDaemon/include/hid_device_streaming_thread.h`
- `NoteDaemon/include/encryption_protocol.h`
- `NoteDaemon/include/note_messaging.h`
- `NoteDaemon/include/capability_registry.h`

---

---

## Architectural Shift: From Fallback to Module-Only

### Current (Transitional) Architecture

```
NoteDaemon Core (Current - WRONG)
├── Socket setup ✓
├── Module loading ✓
├── Message routing
│   ├─ If modules loaded → route to module
│   └─ If NO modules → FALLBACK to DeviceSession ← PROBLEM
└── DeviceSession (should be in module!)
    ├── Handler maps
    ├── USB management
    └── Streaming threads
```

### Target Architecture

```
NoteDaemon Core (Target - CORRECT)
├── Socket setup
├── Module loading/unloading
├── Client connection management
├── Message routing to modules (NO FALLBACK)
└── Just a router - no device handling!

NoteUSB Module
├── DeviceSession (moved here from NoteDaemon)
├── Handler implementations
├── USB device management
├── Streaming threads
└── Device monitor
```

### What Needs to Change

1. **Remove DeviceSession from NoteDaemon**
   - Move `NoteDaemon/src/device_session.cpp` → `NoteUSB/src/`
   - Move `NoteDaemon/include/device_session.h` → `NoteUSB/include/`

2. **Remove Fallback in main.cpp**
   - Current: `if (modules.empty()) use DeviceSession`
   - Should be: `if (modules.empty()) error - no modules loaded`

3. **Add Session Handling to IModule**
   - Add `handle_client(fd, pid)` to IModule interface
   - NoteUSB implements this to create its own session

4. **NoteDaemon Becomes Pure Router**
   - Only handles socket accepting
   - Only routes messages to modules
   - No device handling code

---

## Accessing the Legacy Code

The original NoteDaemon code that these features were extracted from is still available in the NoteDaemon repository. Here's how to access it:

### Direct File Access

The legacy code lives in the NoteDaemon repository at:

```
/home/iospore/Dev/Netnotes/NoteDaemon/
```

This is the code that existed before the modular refactor began. The NoteUSB module is being built to replace parts of this code.

### Key Files to Reference

| Component | Legacy File | Description |
|-----------|-------------|-------------|
| Device Session | `src/device_session.cpp` | Main device handling logic |
| Device Registry | `src/device_registry.cpp` | Registry file read/write |
| Process Monitor | `process-monitor/` | Original monitor (moved to NoteUSB) |
| Headers | `include/device_session.h` | DeviceSession class definition |
| Headers | `include/device_registry.h` | DeviceRegistry class definition |
| Headers | `include/usb_device_descriptor.h` | Device descriptor structure |

### Using Git to Access Historical Code

You can use git to view historical versions or changes:

```bash
# Navigate to NoteDaemon
cd /home/iospore/Dev/Netnotes/NoteDaemon

# See recent changes to device_session.cpp
git log --oneline -10 src/device_session.cpp

# View a specific old version
git show <commit-hash>:src/device_session.cpp | less

# See what changed between commits
git diff <old-commit>..<new-commit> src/device_session.cpp

# Get a specific line range from a historical version
git show <commit>:src/device_session.cpp | sed -n '700,800p'
```

### Current State

The NoteDaemon repository contains:
- **Legacy code**: Original monolithic implementation (still functional)
- **Core framework**: New modular infrastructure (in `include/module_framework/` and `src/core/`)
- **DeviceSession**: Still in place for backward compatibility during transition

The refactor is incremental - you can see the progression by examining git history.

### Finding Specific Code Sections

When implementing features in NoteUSB, you can find the original implementation by:

1. **Searching for function names** in NoteDaemon:
   ```bash
   cd /home/iospore/Dev/Netnotes/NoteDaemon
   grep -n "handle_claim_device" src/device_session.cpp
   grep -n "discover_devices" src/device_session.cpp
   ```

2. **Looking at line numbers** in this document's References section

3. **Using the DeviceRegistry** as an anchor point - it's used by both legacy and new code

### Important Note

The legacy code in NoteDaemon is still actively used - the refactor creates NoteUSB as a separate module that can be loaded alongside or replace parts of the original implementation. The goal is to gradually migrate functionality to the modular structure while keeping the original working until NoteUSB is fully functional.