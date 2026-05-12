# Visual Comparison: Legacy vs Current Architecture

## High-Level Architecture

### Legacy Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         NoteDaemon                             │
│  (Monolithic, Single Process)                                   │
└─────────────────────────────────────────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │                       │
                    ▼                       ▼
        ┌──────────────────┐    ┌──────────────────┐
        │  Main Event Loop │    │  Module Registry │
        │  (select-based)  │    │                  │
        └──────────────────┘    └──────────────────┘
                    │                       │
                    │                       │
                    ▼                       ▼
        ┌──────────────────────────────────────────┐
        │        DeviceSession (per-client)        │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  libusb Context                    │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  Handler Maps (O(1) dispatch)      │ │
        │  │  - control_handlers_               │ │
        │  │  - routed_handlers_                │ │
        │  │  - routed_cmd_handlers_            │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  Device State Map                  │ │
        │  │  device_states[device_id]          │ │
        │  │    → DeviceState                    │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  Streaming Threads Map             │ │
        │  │  streaming_threads[device_id]      │ │
        │  │    → HIDDeviceStreamingThread       │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  Encryption Map                    │ │
        │  │  device_encryptions_[device_id]    │ │
        │  │    → EncryptionHandshake           │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  Device Registry (daemon-wide)     │ │
        │  │  /run/netnotes/device_registry.json│ │
        │  └────────────────────────────────────┘ │
        └──────────────────────────────────────────┘
                    │                       │
                    │                       │
                    ▼                       ▼
        ┌──────────────────┐    ┌──────────────────┐
        │  libusb Hotplug  │    │  Process Monitor │
        │  Callbacks       │    │  (forked binary) │
        └──────────────────┘    └──────────────────┘
```

### Current Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         NoteDaemon                             │
│  (Core Framework + Thin Wrapper)                                │
└─────────────────────────────────────────────────────────────────┘
                                │
                    ┌───────────┴───────────┐
                    │                       │
                    ▼                       ▼
        ┌──────────────────┐    ┌──────────────────┐
        │  ModuleLoader    │    │  ModuleRegistry  │
        │  (dlopen)        │    │                  │
        └──────────────────┘    └──────────────────┘
                    │                       │
                    │                       │
                    ▼                       ▼
        ┌──────────────────────────────────────────┐
        │        ModuleRoutingRegistry             │
        │  (routes by message type → module_id)    │
        └──────────────────────────────────────────┘
                    │
                    │ message_type → module_id
                    │
                    ▼
        ┌──────────────────────────────────────────┐
        │         NoteUSB Module (independent)     │
        │  (built separately as note_usb.so)       │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  IModule Implementation            │ │
        │  │  - name(), version(), description() │ │
        │  │  - init(), start(), stop()          │ │
        │  │  - check_health()                   │ │
        │  │  - capabilities()                   │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  HandlerRegistry (per-module)      │ │
        │  │  - register_module_handler()       │ │
        │  │  - dispatch()                      │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  DeviceHandler                     │ │
        │  │  - discover_devices()              │ │
        │  │  - claim_device()                  │ │
        │  │  - release_device()                │ │
        │  │  - available_devices_ map          │ │
        │  │  - claimed_devices_ map            │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  NoteUSBSession (per-client)       │ │
        │  │  - device_ (single device)         │ │
        │  │  - streaming_thread_               │ │
        │  │  - read_socket()                   │ │
        │  │  - handle_message()                │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  HIDStreamingThread                │ │
        │  │  - capture_thread_                 │ │
        │  │  - process_thread_                 │ │
        │  │  - spsc_queue_                     │ │
        │  │  - events_read_/sent_/dropped_     │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  DeviceRegistry (module-private)   │ │
        │  │  /run/netnotes/modules/note_usb/   │ │
        │  │    device_registry.json            │ │
        │  └────────────────────────────────────┘ │
        │                                          │
        │  ┌────────────────────────────────────┐ │
        │  │  DeviceMonitor (module-specific)   │ │
        │  │  - starts in start()               │ │
        │  │  - survives stop()                 │ │
        │  │  - monitors daemon PID              │ │
        │  │  - reattaches kernel drivers       │ │
        │  └────────────────────────────────────┘ │
        └──────────────────────────────────────────┘
                    │
                    │
                    ▼
        ┌──────────────────┐
        │  libusb Context  │
        │  (module-owned)  │
        └──────────────────┘
```

## Message Flow Comparison

### Legacy Message Flow

```
Client Message
    │
    ▼
┌─────────────────┐
│  readSocket()   │  ← DeviceSession::readSocket()
│                 │
│  Parse message  │
│  (NoteBytes)    │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ dispatch_       │  ← Uses control_handlers_ map
│ control_message │
│                 │
│ O(1) hash lookup│
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ handle_command()│  ← Switch on CMD field
│                 │
│ CLAIM_ITEM      │
│ RELEASE_ITEM    │
│ RESUME          │
│ ...             │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ handle_claim_   │  ← DeviceSession method
│ device()        │
│                 │
│ Open device     │
│ Claim interface │
│ Detach kernel   │
│ Start thread    │
│ Register in     │
│   registry      │
│ Send response   │
└─────────────────┘
```

### Current Message Flow

```
Client Message
    │
    ▼
┌─────────────────┐
│ readSocket()    │  ← NoteUSBSession::readSocket()
│                 │
│ Parse message   │
│ (NoteBytes)     │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ handle_message()│  ← Routes by EVENT/CMD field
│                 │
│ ROUTE TO MODULE │
│                 │
│ (NoteDaemon)    │
│ Core routes     │
│ by message_type │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ NoteUSB Module  │  ← IModule::handle_client()
│                 │
│ get_handler_    │
│ registry().     │
│ dispatch()      │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ register_       │  ← DeviceHandler method
│ module_handler()│
│                 │
│ CLAIM_ITEM      │
│ RELEASE_ITEM    │
│ RESUME          │
│ ...             │
└─────────────────┘
    │
    ▼
┌─────────────────┐
│ handle_claim()  │  ← NoteUSBSession method
│                 │
│ Open device     │
│ Claim interface │
│ Detach kernel   │
│ Start thread    │
│ Register in     │
│   registry      │
│ Send response   │
└─────────────────┘
```

## Data Structures Comparison

### Legacy DeviceSession

```cpp
class DeviceSession {
private:
    // libusb
    libusb_context* usb_ctx;

    // Per-client
    int client_fd;
    pid_t client_pid;

    // Handler maps
    std::unordered_map<NoteBytes::Value, Handler> control_handlers_;
    std::unordered_map<NoteBytes::Value, Handler> routed_handlers_;
    std::unordered_map<NoteBytes::Value, Handler> routed_cmd_handlers_;

    // Device management (map<device_id, DeviceState>)
    std::map<std::string, std::shared_ptr<DeviceState>> device_states;

    // Streaming threads (map<device_id, StreamingThread>)
    std::map<std::string, std::unique_ptr<DeviceStreamingThread>> streaming_threads;

    // Per-device encryption (map<device_id, EncryptionHandshake>)
    std::map<std::string, std::unique_ptr<EncryptionProtocol::EncryptionHandshake>> device_encryptions_;

    // Static session registry (for hotplug broadcasts)
    static std::vector<DeviceSession*>& active_sessions();

    // Device registry (daemon-wide)
    void register_claimed_device(...);
    void unregister_device(...);
};
```

### Current NoteUSB Module

```cpp
class NoteUSBModule : public IModule {
private:
    // Configuration
    nlohmann::json config_;
    int discovery_interval_ms_;
    bool auto_detach_kernel_;

    // State
    bool running_;
    pid_t monitor_pid_;

    // Components
    std::unique_ptr<HandlerRegistry> handler_registry_;
    std::unique_ptr<DeviceHandler> device_handler_;
    libusb_context* usb_ctx_;

    // Active sessions (pid -> NoteUSBSession)
    std::map<pid_t, std::unique_ptr<NoteUSBSession>> sessions_;
};

class DeviceHandler {
private:
    // libusb
    libusb_context* ctx_;

    // Device management
    std::map<std::string, DeviceInfo> claimed_devices_;
    std::map<std::string, std::shared_ptr<USBDeviceDescriptor>> available_devices_;

    // Module-private registry
    static std::mutex& registry_mutex();
};

class NoteUSBSession {
private:
    // libusb
    libusb_context* usb_ctx_;

    // Per-client
    int client_fd_;
    pid_t client_pid_;
    std::atomic<bool> running_;
    std::atomic<bool> stop_requested_;

    // Single device (not map)
    std::shared_ptr<USBDeviceDescriptor> device_;
    std::string device_id_;

    // Streaming thread (single)
    std::unique_ptr<HIDStreamingThread> streaming_thread_;

    // Metrics
    std::atomic<uint64_t> events_read_{0};
    std::atomic<uint64_t> events_sent_{0};
    std::atomic<uint64_t> events_dropped_{0};
};
```

## Streaming Thread Comparison

### Legacy HIDDeviceStreamingThread

```
┌────────────────────────────────────────────────────────────────┐
│ HIDDeviceStreamingThread                                       │
│                                                                │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ capture_thread_  │    │ process_thread_  │                │
│  │                  │    │                  │                │
│  │ libusb event     │    │ NoteBytes writer │                │
│  │ loop             │    │                  │                │
│  │                  │    │ Keyboard parser  │                │
│  │ xfer_callback    │    │                  │                │
│  │                  │    │ Event queue      │                │
│  │ SPSC queue       │    │ (backpressure)   │                │
│  │ (lock-free)      │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
│         │                         │                          │
│         │ HIDReportEvent          │ vector<uint8_t>         │
│         ▼                         ▼                          │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ process_hid_     │    │ queue_event()    │                │
│  │ report()         │    │                  │                │
│  │                  │    │ send_pending_    │                │
│  │ Keyboard parser  │    │ events()         │                │
│  │                  │    │                  │                │
│  │ Event queue      │    │                  │                │
│  │ (deque)          │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
│         │                         │                          │
│         │ vector<uint8_t>         │ write(client_fd)        │
│         ▼                         ▼                          │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ NoteBytes writer │    │ Socket           │                │
│  │ serialize        │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
└────────────────────────────────────────────────────────────────┘

Key Features:
- Encryption support
- Keyboard parsing
- Backpressure queue
- SPSC lock-free transfer
- Per-device encryption handshakes
- Device state tracking
```

### Current HIDStreamingThread

```
┌────────────────────────────────────────────────────────────────┐
│ HIDStreamingThread                                             │
│                                                                │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ capture_thread_  │    │ process_thread_  │                │
│  │                  │    │                  │                │
│  │ libusb event     │    │ NoteBytes writer │                │
│  │ loop             │    │                  │                │
│  │                  │    │ (simple)         │                │
│  │ xfer_callback    │    │                  │                │
│  │                  │    │                  │                │
│  │ SPSC queue       │    │                  │                │
│  │ (lock-free)      │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
│         │                         │                          │
│         │ HIDReportEvent          │ vector<uint8_t>         │
│         ▼                         ▼                          │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ process_hid_     │    │ send_to_client() │                │
│  │ report()         │    │                  │                │
│  │                  │    │                  │                │
│  │ (no parser)      │    │                  │                │
│  │                  │    │                  │                │
│  │ No queue         │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
│         │                         │                          │
│         │ vector<uint8_t>         │ write(client_fd)        │
│         ▼                         ▼                          │
│  ┌──────────────────┐    ┌──────────────────┐                │
│  │ NoteBytes writer │    │ Socket           │                │
│  │ serialize        │    │                  │                │
│  └──────────────────┘    └──────────────────┘                │
└────────────────────────────────────────────────────────────────┘

Key Features:
- No encryption
- No keyboard parser
- No backpressure queue
- SPSC lock-free transfer
- Basic metrics (read/sent/dropped)
- Simple event sending
```

## Device Registry Comparison

### Legacy DeviceRegistry (Daemon-wide)

```
File: /run/netnotes/device_registry.json

[
  {
    "pid": 12345,
    "device_id": "1:2",
    "interface_number": 0,
    "kernel_driver_attached": true
  },
  {
    "pid": 12345,
    "device_id": "1:3",
    "interface_number": 0,
    "kernel_driver_attached": false
  }
]

Access:
- DeviceSession::register_claimed_device()
- DeviceSession::unregister_device()
- DeviceRegistry::get_all_devices()
- DeviceRegistry::remove_all_by_pid()

Scope:
- Daemon-wide
- Accessible by all sessions
- Shared by monitor process
```

### Current DeviceRegistry (Module-private)

```
File: /run/netnotes/modules/note_usb/device_registry.json

[
  {
    "pid": 12345,
    "device_id": "1:2",
    "interface_number": 0,
    "kernel_driver_attached": true
  }
]

Access:
- DeviceHandler::claim_device() → add_device()
- DeviceHandler::release_device() → remove_device()
- DeviceRegistry::get_all_devices()
- DeviceRegistry::remove_all()

Scope:
- Module-private
- Accessible only by module
- Isolated from other modules
- Module-specific monitor process
```

## Hotplug Support Comparison

### Legacy Hotplug

```
┌────────────────────────────────────────────────────────────────┐
│ libusb Hotplug Callbacks (DeviceSession)                       │
│                                                                │
│  static int LIBUSB_CALL hotplug_callback_attached(...)        │
│  {                                                             │
│      if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {      │
│          // Build descriptor                                    │
│          auto device_desc = build_device_descriptor(...);      │
│          if (!device_desc) return 0;                           │
│                                                                 │
│          // Send DEVICE_ATTACHED to all sessions               │
│          send_device_attached(device_id, device_desc);         │
│      }                                                         │
│      return 0;                                                 │
│  }                                                             │
│                                                                │
│  static int LIBUSB_CALL hotplug_callback_detached(...)        │
│  {                                                             │
│      if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {          │
│          // Send DEVICE_DETACHED to all sessions               │
│          send_device_detached(device_id);                      │
│      }                                                         │
│      return 0;                                                 │
│  }                                                             │
│                                                                │
│  void register_hotplug_callbacks(libusb_context* ctx)          │
│  {                                                             │
│      // Register callbacks                                      │
│      libusb_hotplug_register_callback(...);                    │
│      libusb_hotplug_register_callback(...);                    │
│  }                                                             │
└────────────────────────────────────────────────────────────────┘
                         │
                         │
                         ▼
┌────────────────────────────────────────────────────────────────┐
│ Broadcast to All Sessions                                      │
│                                                                │
│  static void broadcast_to_all_sessions(const NoteBytes::Object│
│                                          msg) {                │
│      std::lock_guard<std::mutex> lock(sessions_mutex());       │
│      for (DeviceSession* session : active_sessions()) {        │
│          if (session && session->client_fd >= 0) {             │
│              try {                                             │
│                  session->send_message(msg);                  │
│              } catch (...) {                                   │
│                  // Log warning, continue                     │
│              }                                                 │
│          }                                                     │
│      }                                                         │
│  }                                                             │
└────────────────────────────────────────────────────────────────┘
                         │
                         │
                         ▼
┌────────────────────────────────────────────────────────────────┐
│ Client Receives Notification                                   │
│                                                                │
│  NoteBytes::Object notification;                              │
│  notification.add(NoteMessaging::Keys::EVENT,                 │
│                    NoteMessaging::ProtocolMessages::          │
│                    DEVICE_ATTACHED);                          │
│  notification.add(NoteMessaging::Keys::DEVICE_ID, device_id); │
│  // ... full descriptor                                        │
└────────────────────────────────────────────────────────────────┘
```

### Current Hotplug (NOT IMPLEMENTED)

```
┌────────────────────────────────────────────────────────────────┐
│ libusb Hotplug Callbacks (NOT IMPLEMENTED)                     │
│                                                                │
│  // TODO: Register hotplug callbacks                           │
│  // TODO: Send DEVICE_ATTACHED notifications                   │
│  // TODO: Send DEVICE_DETACHED notifications                   │
└────────────────────────────────────────────────────────────────┘
                         │
                         │
                         ▼
┌────────────────────────────────────────────────────────────────┐
│ Client Receives Notification (NOT IMPLEMENTED)                │
│                                                                │
│  // No hotplug notifications                                   │
│  // Device must be explicitly claimed via REQUEST_DISCOVERY   │
└────────────────────────────────────────────────────────────────┘
```

## Deployment Comparison

### Legacy Deployment

```
┌────────────────────────────────────────────────────────────────┐
│ /etc/netnotes/netnotes.conf (Core config)                      │
├────────────────────────────────────────────────────────────────┤
│ socket.path=/run/netnotes/notedaemon.sock                      │
│ socket.group=netnotes                                          │
│ log.level=info                                                 │
└────────────────────────────────────────────────────────────────┘
                                │
                                │
                                ▼
┌────────────────────────────────────────────────────────────────┐
│ /run/netnotes/                                                 │
│ ├── notedaemon.sock                                            │
│ └── device_registry.json (daemon-wide)                        │
└────────────────────────────────────────────────────────────────┘
                                │
                                │
                                ▼
┌────────────────────────────────────────────────────────────────┐
│ /usr/local/bin/                                                │
│ ├── process_monitor (forked by main)                          │
│ └── notedaemon (monolithic)                                    │
└────────────────────────────────────────────────────────────────┘
```

### Current Deployment

```
┌────────────────────────────────────────────────────────────────┐
│ /etc/netnotes/netnotes.conf (Core config)                      │
├────────────────────────────────────────────────────────────────┤
│ socket.path=/run/netnotes/notedaemon.sock                      │
│ socket.group=netnotes                                          │
│ log.level=info                                                 │
│ modules.directory=/etc/netnotes/modules                       │
│ modules.strict_load=true                                       │
│ modules.health_check=true                                      │
└────────────────────────────────────────────────────────────────┘
                                │
                                │
                                ▼
┌────────────────────────────────────────────────────────────────┐
│ /etc/netnotes/modules/                                         │
│ ├── note_usb/                                                  │
│ │   ├── config.json                                           │
│ │   ├── note_usb.so (module)                                  │
│ │   └── monitor-note_usb (device monitor binary)              │
│ ├── [other modules]                                            │
│ └── device_registry.json (module-private)                     │
└────────────────────────────────────────────────────────────────┘
                                │
                                │
                                ▼
┌────────────────────────────────────────────────────────────────┐
│ /run/netnotes/                                                 │
│ ├── notedaemon.sock                                            │
│ └── modules/note_usb/                                          │
│     └── device_registry.json (module-private)                 │
└────────────────────────────────────────────────────────────────┘
                                │
                                │
                                ▼
┌────────────────────────────────────────────────────────────────┐
│ /usr/local/bin/                                                │
│ ├── process_monitor (forked by main)                          │
│ └── notedaemon (thin wrapper, loads modules)                  │
└────────────────────────────────────────────────────────────────┘
```
