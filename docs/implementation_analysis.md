# NoteUSB Implementation Analysis: Legacy vs Current

## Executive Summary

The NoteUSB module has been successfully extracted from the monolithic NoteDaemon into an independent module project. The current implementation follows the modular architecture defined in `refactor.md`, with the core framework already implemented in NoteDaemon and the NoteUSB module following the two-level routing design.

---

## Architecture Comparison

### Legacy Architecture (NoteDaemon)

```
NoteDaemon (Monolithic)
├── Main Event Loop (select-based)
├── DeviceSession (per-client)
│   ├── libusb context
│   ├── Handler maps (unordered_map)
│   ├── Streaming threads (HIDDeviceStreamingThread)
│   ├── Device registry (DeviceRegistry)
│   ├── Capability registry (bitflags)
│   ├── Encryption protocol
│   └── NoteBytes protocol handling
└── Process monitor (forked, external binary)
```

**Key Characteristics:**
- Single monolithic daemon
- DeviceSession handles all USB/HID operations
- Handler maps for O(1) message dispatch
- Hotplug support via libusb callbacks
- Device registry for crash recovery
- Per-client streaming threads
- Encryption negotiation per device

### Current Architecture (Modular)

```
NoteDaemon Core
├── ModuleLoader              - Runtime loading of .so modules
├── IModule                   - Interface all modules implement
├── ModuleRegistry            - Tracks loaded modules
├── ModuleRoutingRegistry     - Routes messages to modules (Level 1)
├── HandlerRegistry           - Per-module device handlers (Level 2)
├── ErrorCollector            - Pull-based error collection
├── ConfigManager             - Module config loading
└── NoteDaemon (thin wrapper)
    └── NoteUSB Module (independent project)
        ├── IModule implementation
        ├── DeviceHandler (message handlers)
        ├── NoteUSBSession (per-client)
        ├── HIDStreamingThread (USB streaming)
        └── DeviceMonitor (module-specific)
```

**Key Characteristics:**
- Core framework + independent modules
- Two-level routing: Core → Module → Device
- Module-specific DeviceRegistry
- Per-client sessions managed by module
- Module handles device-specific operations
- Device monitor is module-specific

---

## Implementation Status

### NoteDaemon Core Framework

| Component | Status | Notes |
|-----------|--------|-------|
| ModuleLoader | ✅ Complete | dlopen-based, loads .so files |
| IModule interface | ✅ Complete | Lifecycle, health check, capabilities |
| ModuleRegistry | ✅ Complete | Tracks loaded modules, provides lookups |
| HandlerRegistry | ✅ Complete | Per-module handler maps |
| ErrorCollector | ✅ Complete | Pull-based error collection |
| ConfigManager | ✅ Complete | Loads module configs from JSON |
| ModuleRoutingRegistry | ✅ Complete | Routes by message type to modules |

### NoteUSB Module

| Component | Status | Notes |
|-----------|--------|-------|
| IModule implementation | ✅ Complete | `module.cpp` implements all interface methods |
| DeviceHandler | ✅ Complete | Device discovery, claim, release |
| NoteUSBSession | ✅ Complete | Per-client session management |
| HIDStreamingThread | ✅ Complete | Async USB streaming with SPSC queue |
| DeviceRegistry | ✅ Complete | Crash recovery registry |
| DeviceMonitor | ✅ Complete | Monitor binary for reattaching kernel drivers |
| CMakeLists.txt | ✅ Complete | Builds shared library + monitor |
| Tests | ⚠️ Partial | Unit tests exist but incomplete |

### Legacy Code (Still in NoteDaemon)

| Component | Status | Notes |
|-----------|--------|-------|
| DeviceSession | ✅ Complete | Full implementation with hotplug support |
| HIDDeviceStreamingThread | ✅ Complete | Advanced streaming with encryption |
| DeviceRegistry | ✅ Complete | Daemon-wide registry |
| EncryptionProtocol | ✅ Complete | Per-device encryption negotiation |
| libusb hotplug callbacks | ✅ Complete | USB attach/detach notifications |

---

## Key Changes

### 1. Two-Level Routing

**Legacy:**
```
Client Message → DeviceSession (single handler map)
```

**Current:**
```
Client Message → Core (ModuleRoutingRegistry)
              → NoteUSB Module (HandlerRegistry)
              → Device-specific handler
```

**Benefits:**
- Core doesn't need to know about devices
- Modules can have their own namespaces
- Easier to add new modules without modifying core

### 2. DeviceRegistry Scope

**Legacy:**
- `DeviceRegistry` is daemon-wide
- Located in `/run/netnotes/device_registry.json`
- Managed by DeviceSession

**Current:**
- `DeviceRegistry` is module-private
- Located in `/run/netnotes/modules/note_usb/device_registry.json`
- Managed by DeviceHandler

**Benefits:**
- Each module manages its own resources
- No cross-module interference
- Easier to deploy/upgrade modules

### 3. Streaming Thread Implementation

**Legacy:**
```cpp
class HIDDeviceStreamingThread : public DeviceStreamingThread {
    // Advanced features:
    // - Encryption support
    // - Keyboard parser
    // - Event queue with backpressure
    // - SPSC queue for lock-free transfer
    // - Per-device encryption handshakes
};
```

**Current:**
```cpp
class HIDStreamingThread {
    // Basic features:
    // - Async USB streaming
    // - SPSC queue for lock-free transfer
    // - Simple event sending
    // - Device disconnect handling
    // - Metrics tracking
};
```

**Differences:**
- Current lacks encryption support (stubs exist)
- Current lacks keyboard parser (sends raw bytes)
- Current has basic metrics (read/sent/dropped)
- Current doesn't support multiple devices per session

### 4. Hotplug Support

**Legacy:**
```cpp
// Static session registry
static std::vector<DeviceSession*>& active_sessions();

// libusb hotplug callbacks
static int LIBUSB_CALL hotplug_callback_attached(...);
static int LIBUSB_CALL hotplug_callback_detached(...);

// Broadcast to all sessions
static void broadcast_to_all_sessions(const NoteBytes::Object& msg);
```

**Current:**
```cpp
// No hotplug in module yet
// Device discovery only happens on request
// Device discovery happens:
// - When module starts (initial scan)
// - When client requests (handle_discover)
```

**Missing in Current:**
- No libusb hotplug callbacks
- No session registry for broadcasting
- No DEVICE_ATTACHED/DETACHED notifications
- Device must be explicitly claimed

### 5. Device Monitor

**Legacy:**
```cpp
// Process monitor is external
// Forked by main.cpp
// Execs process_monitor binary
// Watches daemon PID
```

**Current:**
```cpp
// Module-specific monitor
// Forked by module.cpp in start()
// Execs monitor-note_usb binary
// Receives module PID
// Located in /etc/netnotes/modules/note_usb/
```

**Benefits:**
- Each module manages its own monitor
- Monitor survives module stop()
- Module-specific cleanup logic

### 6. Session Management

**Legacy:**
```cpp
class DeviceSession {
    // Per-client
    libusb_context* usb_ctx;
    int client_fd;
    pid_t client_pid;

    // Device management (map<device_id, DeviceState>)
    std::map<std::string, std::shared_ptr<DeviceState>> device_states;

    // Streaming threads (map<device_id, HIDDeviceStreamingThread>)
    std::map<std::string, std::unique_ptr<HIDDeviceStreamingThread>> streaming_threads;

    // Encryption (map<device_id, EncryptionHandshake>)
    std::map<std::string, std::unique_ptr<EncryptionHandshake>> device_encryptions_;

    // Handler maps
    std::unordered_map<NoteBytes::Value, Handler> control_handlers_;
    std::unordered_map<NoteBytes::Value, Handler> routed_handlers_;
};
```

**Current:**
```cpp
class NoteUSBSession {
    // Per-client
    libusb_context* usb_ctx;
    int client_fd;
    pid_t client_pid;

    // Single device (not map)
    std::shared_ptr<USBDeviceDescriptor> device_;

    // Streaming thread (single)
    std::unique_ptr<HIDStreamingThread> streaming_thread_;

    // No encryption yet
    // No handler maps (uses module's HandlerRegistry)
};

class DeviceHandler {
    // Device management (map<device_id, DeviceInfo>)
    std::map<std::string, DeviceInfo> claimed_devices_;

    // Device discovery (map<device_id, USBDeviceDescriptor>)
    std::map<std::string, std::shared_ptr<USBDeviceDescriptor>> available_devices_;
};
```

**Differences:**
- Legacy supports multiple devices per session
- Current supports one device per session
- Legacy has encryption per device
- Current has no encryption (stubs)
- Legacy uses handler maps internally
- Current delegates to module's HandlerRegistry

---

## Code Comparison

### Device Discovery

**Legacy (device_session.cpp):**
```cpp
void DeviceSession::discover_devices() {
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(usb_ctx, &device_list);

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = device_list[i];

        // Check if HID device
        if (is_hid_device(device)) {
            uint8_t bus = libusb_get_bus_number(device);
            uint8_t address = libusb_get_device_address(device);
            std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

            // Create descriptor
            auto device_desc = std::make_shared<USBDeviceDescriptor>();
            device_desc->handle = nullptr;
            device_desc->interface_number = 0;
            device_desc->kernel_driver_attached = false;
            device_desc->device_id = device_id;

            available_devices[device_id] = device_desc;
        }
    }

    libusb_free_device_list(device_list, 1);
}
```

**Current (device_handler.cpp):**
```cpp
void DeviceHandler::discover_devices() {
    libusb_device** device_list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &device_list);

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = device_list[i];

        if (!is_hid_device(device)) continue;

        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(device, &desc) != LIBUSB_SUCCESS) continue;

        uint8_t bus = libusb_get_bus_number(device);
        uint8_t address = libusb_get_device_address(device);
        std::string device_id = std::to_string(bus) + ":" + std::to_string(address);

        libusb_device_handle* handle = nullptr;
        if (libusb_open(device, &handle) == LIBUSB_SUCCESS) {
            auto device_desc = std::make_shared<USBDeviceDescriptor>();
            device_desc->device_id = device_id;
            device_desc->vendor_id = desc.idVendor;
            device_desc->product_id = desc.idProduct;
            device_desc->interface_number = 0;
            device_desc->kernel_driver_attached = false;
            device_desc->handle = nullptr;

            available_devices_[device_id] = device_desc;
            libusb_close(handle);
        }
    }

    libusb_free_device_list(device_list, 1);
}
```

**Similarities:**
- Both use libusb to enumerate devices
- Both filter for HID devices
- Both create device descriptors
- Both use bus:address format

**Differences:**
- Current opens device to check permissions
- Current stores vendor_id/product_id
- Current handles permission errors differently

### Device Claiming

**Legacy (device_session.cpp):**
```cpp
void DeviceSession::handle_claim_device(const NoteBytes::Object& msg) {
    std::string device_id = msg.get_string(NoteMessaging::Keys::DEVICE_ID, "");
    std::string correlation_id = msg.get_string(NoteMessaging::Keys::CORRELATION_ID, "");

    // Check device exists
    auto device_it = available_devices.find(device_id);
    if (device_it == available_devices.end()) {
        send_error(..., "Device not found");
        return;
    }

    // Open device
    libusb_device_handle* handle = nullptr;
    libusb_open(usb_device, &handle);

    // Claim interface
    libusb_claim_interface(handle, device_desc->interface_number);

    // Detach kernel driver if needed
    if (libusb_kernel_driver_active(handle, 0) == 1) {
        libusb_detach_kernel_driver(handle, 0);
    }

    // Create streaming thread
    auto streaming_thread = std::make_unique<HIDDeviceStreamingThread>(
        device_desc, device_state, client_fd);
    streaming_thread->start();

    // Register in crash recovery registry
    this->register_claimed_device(device_id, ...);

    // Send success response
    send_message(response);
}
```

**Current (device_handler.cpp):**
```cpp
NoteDaemon::Error DeviceHandler::claim_device(const NoteBytes::Object& msg) {
    std::string device_id = msg.get_string(NoteMessaging::Keys::DEVICE_ID, "");
    syslog(LOG_INFO, "NoteUSB: claim device request for %s", device_id.c_str());

    // Check device exists
    auto device_it = available_devices_.find(device_id);
    if (device_it == available_devices_.end()) {
        return Error::from_code(ErrorCodes::ITEM_NOT_FOUND,
                                "Device not found: " + device_id);
    }

    // Find libusb_device
    libusb_device* usb_device = find_device_by_id(device_id);

    // Open device
    libusb_device_handle* handle = nullptr;
    libusb_open(usb_device, &handle);

    // Claim interface
    libusb_claim_interface(handle, interface_number);

    // Detach kernel driver
    bool kernel_driver_attached = false;
    if (libusb_kernel_driver_active(handle, interface_number) == 1) {
        libusb_detach_kernel_driver(handle, interface_number);
        kernel_driver_attached = true;
    }

    // Store device info
    DeviceInfo info;
    info.device_id = device_id;
    info.handle = handle;
    info.interface_number = interface_number;
    info.kernel_driver_attached = kernel_driver_attached;

    claimed_devices_[device_id] = info;

    // Register with module registry
    DeviceRegistry::add_device(device_id, interface_number, kernel_driver_attached);

    return Error(SUCCESS, "");
}
```

**Similarities:**
- Both open libusb devices
- Both claim interfaces
- Both detach kernel drivers
- Both register in registry

**Differences:**
- Current returns Error instead of sending directly
- Current uses module's HandlerRegistry for responses
- Current delegates response sending to session

### Streaming Thread

**Legacy (hid_device_streaming_thread.h):**
```cpp
class HIDDeviceStreamingThread : public DeviceStreamingThread {
private:
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::shared_ptr<State::DeviceState> device_state_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> paused_{false};

    // Async USB transfer
    libusb_transfer* xfer_ = nullptr;

    // Threads
    std::thread capture_thread_;
    std::thread process_thread_;

    // Lock-free queue (SPSC)
    dro::SPSCQueue<HIDReportEvent> spsc_queue_;

    // Event queue for backpressure
    std::deque<std::vector<uint8_t>> client_queue_;

    // Keyboard parser
    std::unique_ptr<HIDParser::KeyboardParser> keyboard_parser_;

public:
    HIDDeviceStreamingThread(std::shared_ptr<USBDeviceDescriptor> device,
                           std::shared_ptr<State::DeviceState> device_state,
                           int client_fd);

    void start() override;
    void stop() override;
    bool is_running() const override;

private:
    static void LIBUSB_CALL transfer_callback(libusb_transfer* xfer);
    void capture_loop();
    void process_loop();
    void process_hid_report(const uint8_t* data, int length);
    void queue_event(const std::vector<uint8_t>& event_packet);
    void send_pending_events();
    void notify_device_lost();
};
```

**Current (hid_streaming_thread.cpp):**
```cpp
class HIDStreamingThread {
private:
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;
    int client_fd_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Async USB transfer
    libusb_transfer* xfer_ = nullptr;

    // Threads
    std::thread capture_thread_;
    std::thread process_thread_;

    // Lock-free queue (SPSC)
    dro::SPSCQueue<HIDReportEvent> spsc_queue_;

    // Metrics
    std::atomic<uint64_t> events_read_{0};
    std::atomic<uint64_t> events_sent_{0};
    std::atomic<uint64_t> events_dropped_{0};

public:
    HIDStreamingThread(std::shared_ptr<USBDeviceDescriptor> device,
                      const std::string& device_id,
                      int client_fd);

    void start();
    void stop();
    bool is_running() const;

private:
    static void LIBUSB_CALL transfer_callback(libusb_transfer* xfer);
    void capture_loop();
    void process_loop();
    void process_hid_report(const uint8_t* data, int length);
    void send_to_client(const std::vector<uint8_t>& data);
    void notify_device_lost();
};
```

**Similarities:**
- Both use async USB transfer with libusb
- Both have capture and process threads
- Both use SPSC queue for lock-free transfer
- Both handle device disconnects
- Both allocate buffers for transfers

**Differences:**
- Current lacks keyboard parser (sends raw bytes)
- Current lacks backpressure queue (sends directly)
- Current has simple metrics (read/sent/dropped)
- Current doesn't extend a base class
- Current doesn't have encryption support

---

## Missing Features in Current Implementation

### 1. Encryption Support

**Legacy:**
- Per-device encryption handshakes
- EncryptionProtocol class for negotiation
- Encrypted routed messages
- Device encryption state tracking

**Current:**
- Stubs exist in `handle_device_encryption_accept` and `handle_device_encryption_decline`
- No actual encryption implementation
- No encrypted routed messages

### 2. Multiple Devices per Session

**Legacy:**
- DeviceSession supports multiple devices
- Map<device_id, DeviceState>
- Map<device_id, HIDDeviceStreamingThread>

**Current:**
- NoteUSBSession supports only one device
- Single device_ pointer
- Single streaming_thread_

### 3. Hotplug Notifications

**Legacy:**
- libusb hotplug callbacks registered at startup
- DEVICE_ATTACHED/DETACHED sent to all clients
- Static session registry for broadcasting

**Current:**
- No hotplug callbacks
- No DEVICE_ATTACHED/DETACHED notifications
- Device must be explicitly claimed

### 4. Keyboard Parsing

**Legacy:**
- HIDParser::KeyboardParser for key events
- Key code parsing
- Key character events

**Current:**
- No keyboard parser
- Sends raw HID reports

### 5. Backpressure Handling

**Legacy:**
- Client event queue with max size
- Event dropping when queue full
- Event count tracking

**Current:**
- No client event queue
- Sends directly to socket
- No backpressure

### 6. Device State Management

**Legacy:**
- DeviceState with bitflag state
- DeviceFlags (CLAIMED, INTERFACE_CLAIMED, DISCONNECTED, etc.)
- State tracking for each device

**Current:**
- No DeviceState class
- No bitflag state tracking
- Simple device_ pointer

---

## Testing Status

### Legacy Tests (NoteDaemon)

| Test | Status | Notes |
|------|--------|-------|
| DeviceSession tests | ✅ Complete | Full test coverage |
| HID streaming tests | ✅ Complete | Async streaming, backpressure |
| Encryption tests | ✅ Complete | Handshake, encrypt/decrypt |
| Hotplug tests | ✅ Complete | Device attach/detach |
| Keyboard parsing tests | ✅ Complete | Key code parsing |

### Current Tests (NoteUSB)

| Test | Status | Notes |
|------|--------|-------|
| Module interface tests | ⚠️ Partial | Basic tests exist |
| Handler registry tests | ⚠️ Partial | Basic tests exist |
| Configuration tests | ⚠️ Partial | Basic tests exist |
| Device handler tests | ❌ Missing | No device tests |
| Session tests | ❌ Missing | No session tests |
| Streaming tests | ❌ Missing | No streaming tests |
| Monitor tests | ❌ Missing | No monitor tests |

---

## Migration Path

### Completed

1. ✅ Core framework implemented in NoteDaemon
2. ✅ IModule interface created
3. ✅ ModuleLoader implemented
4. ✅ ModuleRegistry implemented
5. ✅ HandlerRegistry implemented
6. ✅ ConfigManager implemented
7. ✅ NoteUSB module extracted as independent project
8. ✅ DeviceHandler implementation
9. ✅ NoteUSBSession implementation
10. ✅ HIDStreamingThread implementation
11. ✅ DeviceRegistry implementation
12. ✅ DeviceMonitor implementation

### In Progress

1. ⚠️ Two-level routing integration
2. ⚠️ Module startup/shutdown sequence
3. ⚠️ Message routing from core to module

### Missing

1. ❌ Encryption implementation
2. ❌ Hotplug support in module
3. ❌ Multiple devices per session
4. ❌ Keyboard parsing
5. ❌ Backpressure handling
6. ❌ Complete test suite
7. ❌ Integration testing
8. ❌ Documentation
9. ❌ Deployment scripts

---

## Recommendations

### High Priority

1. **Implement Encryption API Integration**
   - Use NoteDaemon's EncryptionAPI for per-device encryption
   - Remove stub implementations

2. **Add Hotplug Support**
   - Register libusb hotplug callbacks in module
   - Send DEVICE_ATTACHED/DETACHED to clients
   - Implement session registry for broadcasting

3. **Complete Test Suite**
   - Device discovery tests
   - Device claiming tests
   - Streaming thread tests
   - Monitor tests
   - Integration tests

4. **Add Multiple Device Support**
   - Change NoteUSBSession to support map<device_id, device>
   - Update streaming thread handling

### Medium Priority

5. **Implement Keyboard Parser**
   - Add HIDParser integration
   - Parse key codes, send events

6. **Add Backpressure Handling**
   - Client event queue
   - Event dropping when necessary

7. **Improve Error Handling**
   - Better error codes
   - Error correlation IDs
   - Error recovery

### Low Priority

8. **Performance Optimization**
   - Reduce memory allocations
   - Optimize device discovery
   - Cache device information

9. **Documentation**
   - API documentation
   - Usage examples
   - Deployment guide

10. **Monitoring/Metrics**
    - Device usage metrics
    - Performance metrics
    - Health monitoring

---

## Conclusion

The NoteUSB module has been successfully extracted from the monolithic NoteDaemon and follows the modular architecture defined in `refactor.md`. The current implementation includes:

**Strengths:**
- Clean separation of concerns (module vs core)
- Two-level routing design
- Independent module project
- Device discovery and claiming
- Async USB streaming
- Crash recovery registry
- Module-specific device monitor

**Weaknesses:**
- Missing encryption support
- No hotplug notifications
- Single device per session
- No keyboard parsing
- No backpressure handling
- Incomplete test suite

**Overall Status:**
The module is **~60% complete** relative to the legacy implementation. The core framework is complete, and the module structure is solid. Missing features are primarily encryption, hotplug support, and advanced streaming features.

**Next Steps:**
1. Implement encryption API integration
2. Add hotplug support
3. Complete test suite
4. Add multiple device support
5. Implement keyboard parsing
