# Device Acquisition Simulation

## Overview

This document simulates the complete process of acquiring a USB device through the NoteUSB module, tracing the code path from client connection to device streaming.

---

## Scenario

**Client:** A NoteNotes client application wants to acquire a USB HID device
**Device:** USB keyboard (bus:address = "1:2")
**Module:** NoteUSB

---

## Phase 1: Client Connection

### 1.1 Client Connects to Daemon

```
Client Application
    │
    ▼
socket.connect("/run/netnotes/notedaemon.sock")
    │
    ▼
┌─────────────────────────────────────┐
│ NoteDaemon Main Loop (select)       │
│                                      │
│ fd_set ready                         │
│   └─ client_fd = 5                   │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ handle_client(client_fd=5)          │
│                                      │
│ Check: Does module exist?            │
│   └─ Yes → NoteUSB module           │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::handle_client()      │
│                                      │
│ Check: Session exists for pid?       │
│   └─ No → Create new session        │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::NoteUSBSession()    │
│                                      │
│ Parameters:                          │
│   - usb_ctx: libusb context          │
│   - client_fd: 5                     │
│   - client_pid: 12345                │
│   - device_id: "" (empty)            │
│                                      │
│ Actions:                             │
│   1. syslog: "created for pid=12345"│
│   2. register_for_hotplug()          │
│      └─ SessionManager::register_session(5, 12345)
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::start()             │
│                                      │
│ Check: Is already running?           │
│   └─ No → Set running_=true         │
│                                      │
│ Check: device_id empty?              │
│   └─ Yes → Discovery only           │
│                                      │
│ Actions:                             │
│   1. read_socket()                   │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::read_socket()       │
│                                      │
│ Loop:                                │
│   1. Read message type               │
│   2. Read message body               │
│   3. handle_message()                │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_message()    │
│                                      │
│ Parse message:                       │
│   {                                  │
│     "event": "request_discovery"     │
│   }                                  │
│                                      │
│ Route by event type:                 │
│   └─ "request_discovery" →           │
│      handle_discover()               │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_discover()   │
│                                      │
│ Action: discover_devices()          │
│   └─ Scan USB devices                │
│   └─ Find HID devices                │
│   └─ Return device list             │
│                                      │
│ Send response:                       │
│   {                                  │
│     "event": "item_list",            │
│     "items": [                       │
│       {                              │
│         "device_id": "1:2",          │
│         "vendor_id": 1234,           │
│         "product_id": 5678           │
│       }                              │
│     ]                                │
│   }                                  │
└─────────────────────────────────────┘
    │
    ▼
Client receives device list
    │
    ▼
Client selects device "1:2" and sends:
    {
      "event": "claim_item",
      "device_id": "1:2",
      "correlation_id": "abc123"
    }
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_message()    │
│                                      │
│ Parse message:                       │
│   {                                  │
│     "event": "claim_item",           │
│     "device_id": "1:2",              │
│     "correlation_id": "abc123"       │
│   }                                  │
│                                      │
│ Route by event type:                 │
│   └─ "claim_item" → handle_claim()   │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_claim()      │
│                                      │
│ Extract parameters:                  │
│   - device_id: "1:2"                 │
│   - correlation_id: "abc123"         │
│                                      │
│ Action: claim_device("1:2")         │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::claim_device()      │
│                                      │
│ Steps:                               │
│   1. Find libusb_device with id "1:2"│
│   2. libusb_open(device)             │
│   3. libusb_claim_interface(handle, 0)│
│   4. libusb_detach_kernel_driver()   │
│   5. Build device descriptor        │
│   6. Start streaming thread          │
│   7. Register in module registry     │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::start_streaming()   │
│                                      │
│ Create HIDStreamingThread:          │
│   - device: device descriptor        │
│   - device_id: "1:2"                 │
│   - client_fd: 5                     │
│   - session: this                    │
│                                      │
│ Call: set_session(this)              │
│                                      │
│ Call: start()                        │
│   └─ Create capture thread          │
│   └─ Create process thread          │
│   └─ libusb_submit_transfer()       │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ HIDStreamingThread::start()         │
│                                      │
│ Create threads:                      │
│   1. capture_thread_                 │
│   2. process_thread_                 │
│                                      │
│ Setup libusb transfer:               │
│   - libusb_fill_interrupt_transfer() │
│   - libusb_submit_transfer()         │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ HIDStreamingThread::capture_loop()  │
│                                      │
│ libusb_event_loop:                   │
│   - libusb_handle_events_timeout()   │
│   - Transfer callback fired         │
│                                      │
│ Transfer callback:                  │
│   - LIBUSB_TRANSFER_COMPLETED        │
│   - Read HID report from device      │
│   - Push to SPSC queue               │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ HIDStreamingThread::process_loop()  │
│                                      │
│ Loop:                                │
│   1. Pop from SPSC queue             │
│   2. process_hid_report()           │
│   3. encrypt if enabled              │
│   4. send_to_client()                │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_claim()      │
│                                      │
│ Send success response:               │
│   {                                  │
│     "event": "item_claimed",         │
│     "device_id": "1:2",              │
│     "correlation_id": "abc123",      │
│     "status": "claimed"              │
│   }                                  │
└─────────────────────────────────────┘
    │
    ▼
Client receives: "device claimed: 1:2"
    │
    ▼
Client sends: "resume" message
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_resume()     │
│                                      │
│ Acknowledge:                        │
│   {                                  │
│     "event": "resume",               │
│     "device_id": "1:2",              │
│     "status": "ok"                   │
│   }                                  │
└─────────────────────────────────────┘
    │
    ▼
Streaming is now active!
    │
    ▼
Client receives HID reports from device
    │
    ▼
Client sends: "release_item" message
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_release()    │
│                                      │
│ Extract parameters:                  │
│   - device_id: "1:2"                 │
│   - correlation_id: "def456"         │
│                                      │
│ Action: release_device()             │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::release_device()    │
│                                      │
│ Steps:                               │
│   1. Stop streaming thread           │
│   2. libusb_release_interface()      │
│   3. libusb_attach_kernel_driver()   │
│   4. libusb_close(handle)            │
│   5. Clear device pointer            │
│                                      │
│ Note: Session is NOT destroyed       │
│       Session stays open             │
│       Device can be re-claimed       │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_release()    │
│                                      │
│ Send success response:               │
│   {                                  │
│     "event": "item_released",        │
│     "device_id": "1:2",              │
│     "correlation_id": "def456",      │
│     "status": "success"              │
│   }                                  │
└─────────────────────────────────────┘
    │
    ▼
Client receives: "device released"
    │
    ▼
Session stays open (NOT destroyed)
    │
    ▼
Client can now send: "claim_item" for a different device
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_claim()      │
│                                      │
│ Claim new device "1:3"               │
│   - Stop previous streaming          │
│   - Claim "1:3"                      │
│   - Start new streaming              │
│                                      │
│ Send success response                │
└─────────────────────────────────────┘
    │
    ▼
Client receives: "device claimed: 1:3"
    │
    ▼
Streaming active for device "1:3"
    │
    ▼
Client disconnects
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::~NoteUSBSession()   │
│                                      │
│ Actions:                             │
│   1. unregister_from_hotplug()       │
│      └─ SessionManager::unregister_session(5)
│                                      │
│ 2. stop_streaming()                  │
│                                      │
│ Note: Device NOT released here       │
│       (stays claimed for re-use)     │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::cleanup_client()     │
│                                      │
│ Check: Session exists for pid?       │
│   └─ Yes → Remove from sessions_     │
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteDaemon Main Loop                │
│                                      │
│ select() returns 0 (client closed)  │
│                                      │
│ Call: handle_client()                │
│                                      │
│ Check: Can client be routed?         │
│   └─ No (no module loaded)           │
│                                      │
│ Call: cleanup_client(12345)          │
│   └─ Remove from sessions_           │
│                                      │
│ Close client_fd = 5                  │
│                                      │
│ Return to select()                   │
└─────────────────────────────────────┘
    │
    ▼
End of connection
```

---

## Phase 2: Hotplug Device Attachment

### 2.1 USB Device Plugged In

```
User plugs in USB keyboard (bus:address = "1:3")

┌─────────────────────────────────────┐
│ Linux Kernel                        │
│                                      │
│ USB subsystem detects device         │
│                                      │
│ Calls: libusb_hotplug_callback_attached()
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::hotplug_callback_attached()
│                                      │
│ Parameters:                          │
│   - ctx: libusb context              │
│   - device: libusb_device*           │
│   - event: LIBUSB_HOTPLUG_EVENT_ARRIVED
│   - user_data: this                  │
│                                      │
│ Check: Is device attached?           │
│   └─ Yes → Process                   │
│                                      │
│ Actions:                             │
│   1. Get bus and address             │
│   2. Create device_id = "1:3"        │
│   3. Build device descriptor         │
│   4. Check if HID device             │
│                                      │
│ Call: send_device_attached("1:3", descriptor)
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::send_device_attached()
│                                      │
│ Create notification:                 │
│   {                                  │
│     "event": "device_attached",      │
│     "device_id": "1:3",              │
│     "item_info": {                   │
│       "device_id": "1:3",            │
│       "vendor_id": 1234,             │
│       "product_id": 5678             │
│     }                                │
│   }                                  │
│                                      │
│ Call: broadcast_to_all_sessions(notification)
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ SessionManager::broadcast_to_all_sessions()
│                                      │
│ Lock mutex                            │
│                                      │
│ For each session in sessions_:       │
│   - session->client_fd >= 0?         │
│   - Send message via send_message()  │
│                                      │
│ Return: sent_count                   │
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ SessionManager::send_message()      │
│                                      │
│ Serialize message:                   │
│   data = msg.serialize_with_header()
│                                      │
│ Send to socket:                      │
│   write(client_fd, data.data(), size)
│                                      │
│ Return: SUCCESS                      │
└─────────────────────────────────────┘
    │
    ▼
All connected clients receive:
    {
      "event": "device_attached",
      "device_id": "1:3",
      "item_info": {
        "device_id": "1:3",
        "vendor_id": 1234,
        "product_id": 5678
      }
    }
    │
    ▼
Client applications see new device
    │
    ▼
Client sends: "claim_item" for "1:3"
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_claim()      │
│                                      │
│ Claim device "1:3"                   │
│                                      │
│ Send success response                │
└─────────────────────────────────────┘
    │
    ▼
Streaming active for device "1:3"
```

---

## Phase 3: Hotplug Device Detachment

### 3.1 USB Device Unplugged

```
User unplugs USB keyboard (bus:address = "1:3")

┌─────────────────────────────────────┐
│ Linux Kernel                        │
│                                      │
│ USB subsystem detects device gone    │
│                                      │
│ Calls: libusb_hotplug_callback_detached()
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::hotplug_callback_detached()
│                                      │
│ Parameters:                          │
│   - ctx: libusb context              │
│   - device: libusb_device*           │
│   - event: LIBUSB_HOTPLUG_EVENT_LEFT
│   - user_data: this                  │
│                                      │
│ Check: Is device detached?           │
│   └─ Yes → Process                   │
│                                      │
│ Actions:                             │
│   1. Get bus and address             │
│   2. Create device_id = "1:3"        │
│   3. Send device detached notification
└─────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBModule::send_device_detached()
│                                      │
│ Create notification:                 │
│   {                                  │
│     "event": "device_detached",      │
│     "device_id": "1:3"               │
│   }                                  │
│                                      │
│ Call: broadcast_to_all_sessions(notification)
└─────────────────────────────────────┘
    │
    ▼
All connected clients receive:
    {
      "event": "device_detached",
      "device_id": "1:3"
    }
    │
    ▼
Client applications see device disconnected
    │
    ▼
Client sends: "release_item" for "1:3"
    │
    ▼
┌─────────────────────────────────────┐
│ NoteUSBSession::handle_release()    │
│                                      │
│ Release device "1:3"                 │
│                                      │
│ Send success response                │
└─────────────────────────────────────┘
    │
    ▼
Device released
    │
    ▼
Session stays open (NOT destroyed)
    │
    ▼
Device can be re-plugged and re-claimed
```

---

## Code Flow Summary

### Client Connection Flow

```
Client connects
    ↓
NoteDaemon::handle_client()
    ↓
NoteUSBModule::handle_client()
    ↓
NoteUSBSession::NoteUSBSession()
    ├─ register_for_hotplug()
    └─ start()
        ↓
    read_socket()
        ↓
    handle_message()
        ↓
    handle_discover() → Send device list
        ↓
Client sends claim_item
        ↓
    handle_claim()
        ↓
    claim_device()
        ├─ libusb_open()
        ├─ libusb_claim_interface()
        ├─ libusb_detach_kernel_driver()
        ├─ start_streaming()
        │   ├─ HIDStreamingThread::start()
        │   │   ├─ capture_thread_
        │   │   ├─ process_thread_
        │   │   └─ libusb_submit_transfer()
        │   └─ set_session(this)
        └─ Send success response
```

### Device Streaming Flow

```
HIDStreamingThread::start()
    ↓
    ├─ capture_thread_ (libusb event loop)
    │   ↓
    │   transfer_callback()
    │   ├─ LIBUSB_TRANSFER_COMPLETED
    │   ├─ Read HID report
    │   └─ Push to SPSC queue
    │
    └─ process_thread_
        ↓
        process_hid_report()
            ├─ Create NoteBytes event
            ├─ encrypt_data() (if enabled)
            └─ send_to_client()
                └─ write(client_fd, data)
```

### Device Release Flow

```
Client sends release_item
    ↓
    handle_release()
        ↓
    release_device()
        ├─ stop_streaming()
        │   └─ streaming_thread_->stop()
        ├─ libusb_release_interface()
        ├─ libusb_attach_kernel_driver()
        └─ libusb_close(handle)
        ↓
    Send success response
        ↓
NoteUSBSession::stop()
    ├─ stop_streaming()
    └─ (device NOT released - stays claimed)
        ↓
Session stays open (NOT destroyed)
    ↓
Device can be re-claimed later
```

### Hotplug Flow

```
USB device attached
    ↓
libusb_hotplug_callback_attached()
    ↓
    ├─ Get bus:address
    ├─ Build device descriptor
    ├─ send_device_attached()
    └─ broadcast_to_all_sessions()
        ↓
        For each session:
            └─ send_message()
                └─ write(client_fd, notification)
        ↓
All clients receive DEVICE_ATTACHED
        ↓
Client can claim new device
```

### Client Disconnect Flow

```
Client disconnects
    ↓
NoteDaemon::handle_client() returns
    ↓
NoteUSBModule::cleanup_client()
    ↓
    ├─ sessions_.erase(pid)
    └─ SessionManager::unregister_session(fd)
        ↓
NoteUSBSession::~NoteUSBSession()
    ├─ unregister_from_hotplug()
    └─ stop_streaming()
        ↓
Session destroyed
    ↓
Device stays claimed (not released)
```

---

## Key Design Decisions

### 1. Session Stays Open After Device Release

**Why:**
- Routing remains locked in (as requested)
- Session can immediately claim a new device
- No need to reconnect to claim another device
- More efficient for multi-device workflows

**Implementation:**
```cpp
void NoteUSBSession::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    
    stop_streaming();
    
    // Note: We don't release the device here
    // The session stays open so the client can claim a new device later
    // The device will be released when the client disconnects or explicitly
    // requests a device release.
    
    syslog(LOG_INFO, "NoteUSBSession: stopped for client pid=%d", client_pid_);
}
```

### 2. One Device Per Session

**Why:**
- Simplifies routing (session is locked to device)
- Easier to implement encryption (one key per session)
- Clear ownership model
- Prevents race conditions between multiple devices

**Limitation:**
- Multiple devices per session not supported
- Client must release current device before claiming another

**Future Work:**
- Could extend to support map<device_id, streaming_thread>
- Would require more complex routing

### 3. Hotplug Notifications to All Sessions

**Why:**
- All clients should be aware of device changes
- Enables real-time device discovery
- Consistent experience across clients

**Implementation:**
```cpp
static void broadcast_to_all_sessions(const NoteBytes::Object& msg) {
    std::lock_guard<std::mutex> lock(sessions_mutex());
    
    for (DeviceSession* session : active_sessions()) {
        if (session && session->client_fd >= 0) {
            try {
                session->send_message(msg);
            } catch (const std::exception& e) {
                syslog(LOG_WARNING, "Failed to broadcast to session: %s", e.what());
                session->is_active = false;
            }
        }
    }
}
```

### 4. SPSC Queue for Streaming

**Why:**
- Lock-free performance (capture → process)
- Low latency
- No mutex contention

**Implementation:**
```cpp
dro::SPSCQueue<HIDReportEvent> spsc_queue_{HidConstants::kSpscQueueCapacity};
```

### 5. Device Registry for Crash Recovery

**Why:**
- Monitor process can reattach kernel drivers after daemon crash
- Prevents device being stuck in detached state
- Improves reliability

**Implementation:**
```cpp
class DeviceRegistry {
    static void add_device(const std::string& device_id, int interface_number, bool kernel_driver_attached);
    static void remove_device(const std::string& device_id);
    static std::vector<ClaimedDevice> get_all_devices();
    static void remove_all();
};
```

---

## Error Handling

### Device Not Found

```
Client sends: claim_item for non-existent device "99:99"
    ↓
handle_claim()
    ↓
find_device_by_id("99:99") → nullptr
    ↓
Send error response:
    {
      "event": "item_claimed",
      "device_id": "99:99",
      "error_code": 10,  // ITEM_NOT_FOUND
      "message": "Device not found: 99:99",
      "correlation_id": "xyz789"
    }
```

### Device Already Claimed

```
Client A claims device "1:2"
    ↓
Client B tries to claim "1:2"
    ↓
handle_claim()
    ↓
Check: Is device already claimed?
    └─ Yes → Send error:
        {
          "event": "item_claimed",
          "device_id": "1:2",
          "error_code": 11,  // ITEM_NOT_AVAILABLE
          "message": "Device already claimed: 1:2",
          "correlation_id": "abc123"
        }
```

### Device Disconnected During Streaming

```
libusb reports: LIBUSB_TRANSFER_NO_DEVICE
    ↓
HIDStreamingThread::notify_device_lost()
    ↓
    ├─ Stop streaming thread
    ├─ Send DEVICE_DISCONNECTED to client
    └─ Keep session alive (not destroy)
        ↓
Client receives:
    {
      "event": "device_disconnected",
      "device_id": "1:2",
      "message": "USB device physically disconnected"
    }
        ↓
Session stays open (NOT destroyed)
    ↓
Client can re-claim device after re-attachment
```

---

## Performance Considerations

### 1. Lock-Free SPSC Queue

- Capture thread writes to queue
- Process thread reads from queue
- No mutex contention
- Minimal cache line bouncing

### 2. Hotplug Callbacks

- Runs in libusb's context (non-blocking)
- Fast lookup of sessions
- No blocking operations
- Minimal overhead

### 3. Session Management

- Global session registry for hotplug
- Fast O(1) session lookup
- Minimal synchronization

### 4. Memory Management

- Smart pointers for device descriptors
- RAII for libusb handles
- No manual memory management

---

## Security Considerations

### 1. Encryption Support

- Per-session encryption keys
- Encryption API integration
- Data encrypted before sending to client
- No plaintext data in memory

### 2. Permission Checks

- libusb_open() checks permissions
- Device interface claiming
- Kernel driver detachment

### 3. Client Authentication

- Unix socket with group permissions
- getpeercred() for client verification
- Session per client PID

---

## Testing Checklist

### Basic Functionality

- [ ] Client connects and receives device list
- [ ] Client claims a device successfully
- [ ] Client receives HID reports from device
- [ ] Client releases a device successfully
- [ ] Client disconnects and session is cleaned up

### Hotplug

- [ ] Device attachment triggers notification
- [ ] Device detachment triggers notification
- [ ] Multiple clients receive notifications
- [ ] Client can claim newly attached device
- [ ] Client can release device after detachment

### Session Management

- [ ] Session stays open after device release
- [ ] Session can claim a new device
- [ ] Session is destroyed only on client disconnect
- [ ] Session unregisters from hotplug on destruction

### Error Handling

- [ ] Error response for device not found
- [ ] Error response for device already claimed
- [ ] Error response for invalid device_id
- [ ] Graceful handling of device disconnection

### Encryption

- [ ] Encryption can be enabled for a device
- [ ] Encryption can be disabled
- [ ] Encrypted data is sent to client
- [ ] Decrypted data is received from client

---

## Future Enhancements

### 1. Multiple Devices Per Session

**Current:**
- One device per session
- Session stays open after release

**Future:**
- Map<device_id, streaming_thread>
- Support multiple devices simultaneously
- More complex routing

### 2. Device Prioritization

**Current:**
- First device in list is discovered first

**Future:**
- Client can prioritize devices
- Device ranking based on user preference

### 3. Batch Device Claims

**Current:**
- One device at a time

**Future:**
- Claim multiple devices in one message
- Parallel streaming for multiple devices

### 4. Device Hot-Reloading

**Current:**
- Device must be released and re-claimed

**Future:**
- Automatic device re-claim on hotplug
- Seamless device switching

---

## Conclusion

This simulation demonstrates the complete device acquisition flow from client connection to device streaming. The key design decisions are:

1. **Session stays open after device release** - Enables immediate device re-claim
2. **One device per session** - Simplifies routing and encryption
3. **Hotplug notifications to all sessions** - Real-time device discovery
4. **Lock-free SPSC queue** - Low latency streaming
5. **Device registry** - Crash recovery for kernel driver reattachment

The implementation provides a solid foundation for secure, low-latency USB device streaming with hotplug support.
