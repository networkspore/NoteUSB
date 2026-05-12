# NoteUSB Architecture - Current vs Future

## Overview

This document describes the current state of the NoteUSB module and NoteDaemon after refactoring, and outlines the target architecture we're working toward.

---

## Current State (Transitional)

### What Works

| Component | Status | Notes |
|-----------|--------|-------|
| NoteDaemon Core Framework | ✓ Working | Module loading, routing infrastructure |
| NoteUSB Module Build | ✓ Working | Produces `note_usb.so` |
| NoteUSB Monitor Build | ✓ Working | Produces `note_usb_monitor` |
| Module Discovery | ✓ Working | Scans `/etc/netnotes/modules/` |
| Module Lifecycle | ✓ Working | init/start/stop/shutdown |
| Device Monitor | ✓ Working | Reattaches kernels on daemon exit |

### What's Stub

| Component | Status | Notes |
|-----------|--------|-------|
| Device Discovery | Stub | Just logs, doesn't scan USB |
| Device Claim | Stub | Just logs, doesn't claim device |
| Device Release | Stub | Just logs, doesn't release device |
| Message Handlers | Stub | Just log messages, don't process |
| Session Management | In NoteDaemon | DeviceSession still in NoteDaemon |

### Architectural Issue

```
Current NoteDaemon Architecture:

┌─────────────────────────────────────────────┐
│              NoteDaemon Core                │
├─────────────────────────────────────────────┤
│  Socket setup                              │
│  Module loading                             │
│  Message routing                            │
│                                             │
│  FALLBACK: If no modules → DeviceSession   │ ← PROBLEM
│       └── Handles ALL device logic          │
└─────────────────────────────────────────────┘
        ↓ (fallback)
┌─────────────────────────────────────────────┐
│           DeviceSession                     │  ← Should be in module!
│  • Handler maps (claim/release)            │
│  • USB device management                    │
│  • Streaming threads                        │
│  • Hotplug callbacks                        │
└─────────────────────────────────────────────┘
```

The problem: DeviceSession is still in NoteDaemon, not in the module.

---

## Target Architecture

### NoteDaemon Core (Router Only)

```
┌─────────────────────────────────────────────┐
│              NoteDaemon Core               │
├─────────────────────────────────────────────┤
│  Socket creation                           │
│  Client acceptance                          │
│  Client credentials (uid/gid/pid)          │
│  Module loading/unloading                  │
│  Message routing → modules                 │
│  Error handling                            │
│                                             │
│  NO FALLBACK - pure router                  │
│  NO device handling                        │
└─────────────────────────────────────────────┘
                         ↓ message routing
                         ↓ (based on message type)
┌─────────────────────────────────────────────┐
│             NoteUSB Module                  │
├─────────────────────────────────────────────┤
│  IModule Implementation                    │
│  • name(), version(), capabilities()        │
│  • init(), start(), stop(), shutdown()     │
│  • get_handled_message_types()             │
│  • handle_client() ← NEW                   │
│                                             │
│  NoteUSBSession (from DeviceSession)       │
│  • Handler maps                            │
│  • Device management                       │
│  • Streaming threads                       │
│  • Hotplug callbacks                       │
│                                             │
│  NoteUSBDeviceMonitor                      │
│  • Reattaches kernel drivers               │
└─────────────────────────────────────────────┘
```

### Module Interface Changes

The IModule interface needs to add session handling:

```cpp
class IModule {
    // ... existing methods ...
    
    // NEW: Handle a client connection
    virtual void handle_client(int client_fd, pid_t client_pid) = 0;
};
```

### NoteUSB Implements Session Handling

```cpp
class NoteUSBModule : public NoteDaemon::IModule {
public:
    void handle_client(int client_fd, pid_t client_pid) override {
        // This replaces DeviceSession
        NoteUSBSession session(this, client_fd, client_pid);
        session.readSocket();  // Main message loop
    }
};
```

---

## Message Flow Comparison

### Current (With Fallback)

```
Client → NoteDaemon socket
    ↓
NoteDaemon: accept() + get credentials
    ↓
Message type?
    ↓
├─ Modules loaded? → Route to module
└─ No modules? → Use DeviceSession (fallback)
        ↓
    DeviceSession handles everything
```

### Target (Module-Only)

```
Client → NoteDaemon socket
    ↓
NoteDaemon: accept() + get credentials
    ↓
Parse message → determine type
    ↓
Lookup module for message type
    ↓
Route to module
    ↓
Module: handle_client(client_fd, pid)
    ↓
NoteUSBSession: readSocket()
    ↓
Process with handlers (claim/release/etc)
```

---

## Key Differences

| Aspect | Current | Target |
|--------|---------|--------|
| Device handling location | DeviceSession in NoteDaemon | NoteUSB module |
| Fallback behavior | Yes (uses DeviceSession) | No (error if no modules) |
| Session management | NoteDaemon creates DeviceSession | Module creates its own session |
| Module responsibility | Partial - just registers handlers | Full - handles entire client |
| NoteDaemon role | Partial router, partial handler | Pure router |

---

## Migration Steps

1. **Move DeviceSession to NoteUSB**
   - Copy `src/device_session.cpp` → `NoteUSB/src/`
   - Copy `include/device_session.h` → `NoteUSB/include/`
   - Rename to NoteUSBSession

2. **Update IModule Interface**
   - Add `handle_client(int client_fd, pid_t client_pid)` method

3. **Update NoteUSB Module**
   - Implement `handle_client()` using NoteUSBSession

4. **Update NoteDaemon main.cpp**
   - Remove DeviceSession include
   - Remove fallback code
   - Always route to module (error if none)

5. **Clean Up**
   - Remove DeviceSession from NoteDaemon (after verifying NoteUSB works)

---

## Summary

| Phase | Description |
|-------|-------------|
| Current | DeviceSession in NoteDaemon with fallback |
| Transition | NoteUSB has stubs, DeviceSession still in NoteDaemon |
| Target | NoteDaemon is pure router, NoteUSB handles everything |

The goal is to have NoteDaemon be a minimal router that loads modules and routes messages to them, with all device handling completely in the module. DeviceSession should move from NoteDaemon to NoteUSB, and NoteDaemon should have no fallback behavior.