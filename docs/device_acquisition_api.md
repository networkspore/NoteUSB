# NoteUSB Device Acquisition API

## Overview

This document describes the NoteUSB module's implementation of the device acquisition protocol. NoteUSB handles USB HID devices (keyboards, mice, gamepads, etc.) and provides event streaming to clients.

---

## Module Information

| Property | Value |
|----------|-------|
| Module Name | `note_usb` |
| Module Version | `1.0.0` |
| Description | USB HID device support for NoteDaemon |
| Capabilities | `0x0001` (HID devices) |

### Handled Message Types

- `claim_item` - Claim a USB device
- `release_item` - Release a claimed USB device
- `key_down` - Keyboard key pressed
- `key_up` - Keyboard key released
- `key_repeat` - Keyboard key repeat (held down)
- `mouse_move_rel` - Relative mouse movement
- `mouse_button_down` - Mouse button pressed
- `mouse_button_up` - Mouse button released
- `mouse_scroll` - Mouse scroll wheel

---

## Device Discovery

### Request Format

```json
{
  "event": "cmd",
  "cmd": "request_discovery"
}
```

### Response Format

NoteUSB scans the USB bus for HID devices and returns:

```json
{
  "event": "cmd",
  "cmd": "item_list",
  "items": [
    {
      "device_id": "1:2",
      "vendor_id": 0x1234,
      "product_id": 0x5678,
      "manufacturer": "Example Corp",
      "product": "USB Keyboard",
      "serial_number": "ABC123",
      "bus_number": 1,
      "item_address": 2,
      "item_type": "HID"
    }
  ]
}
```

### Discovery Process

1. NoteDaemon receives `request_discovery` command
2. NoteUSB (via DeviceSession) calls `libusb_get_device_list()`
3. For each USB device:
   - Get device descriptor (VID, PID)
   - Check if device is a HID device (Interface Class 3)
   - Try to open device to verify permissions
   - Create USBDeviceDescriptor
4. Return list of available HID devices

### Device ID Format

Device IDs are in `bus:address` format:
- `bus` - USB bus number (1-255)
- `address` - Device address on bus (1-127)

Example: `"1:2"` means bus 1, device address 2

---

## Device Claiming

### Request Format

```json
{
  "event": "cmd",
  "cmd": "claim_item",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid"
}
```

### Claim Process

1. **Validate Request**
   - Check device_id is provided
   - Check correlation_id is provided (for response correlation)

2. **Check Device Availability**
   - Look up device in `available_devices` map
   - Check device is not already in `device_states` map (not claimed)

3. **Open USB Device**
   - Call `libusb_open()` to get device handle
   - If fails with permission error, return `PERMISSION_DENIED`

4. **Claim Interface**
   - Call `libusb_claim_interface(handle, interface_number)`
   - Usually interface 0 for HID devices

5. **Detach Kernel Driver** (if active)
   - Check if kernel driver is active: `libusb_kernel_driver_active()`
   - If active, detach: `libusb_detach_kernel_driver()`
   - Store flag to reattach on release

6. **Create Device State**
   - Create DeviceState object with:
     - device_id
     - owner_pid (client's process ID)
     - device_type = "HID"
     - available_capabilities from Capabilities::Masks::mode_mask()

7. **Start Streaming Thread**
   - Create HIDDeviceStreamingThread
   - Pass device descriptor, device state, client socket
   - Call `thread->start()`

8. **Register for Crash Recovery**
   - Store device in crash-recovery registry
   - Record interface number and kernel driver status

### Response: Success

```json
{
  "event": "item_claimed",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid",
  "status": "claimed"
}
```

### Response: Errors

**Device Not Found (101)**
```json
{
  "event": "item_claimed",
  "device_id": "99:99",
  "correlation_id": "client-generated-uuid",
  "error_code": 101,
  "msg": "Device not found: 99:99"
}
```

**Device Already Claimed (102)**
```json
{
  "event": "item_claimed",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid",
  "error_code": 102,
  "msg": "Device already claimed: 1:2"
}
```

**Permission Denied (103)**
```json
{
  "event": "item_claimed",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid",
  "error_code": 103,
  "msg": "Cannot open device 1:2: LIBUSB_ERROR_ACCESS"
}
```

---

## Event Streaming

Once a device is claimed, HID events are streamed to the client.

### Keyboard Events

**Key Down**
```json
{
  "event": "key_down",
  "device_id": "1:2",
  "key_code": 0x04,
  "timestamp": 1699999999000
}
```

**Key Up**
```json
{
  "event": "key_up",
  "device_id": "1:2",
  "key_code": 0x04,
  "timestamp": 1699999999100
}
```

**Key Repeat**
```json
{
  "event": "key_repeat",
  "device_id": "1:2",
  "key_code": 0x04,
  "timestamp": 1699999999200
}
```

### Mouse Events

**Mouse Move (Relative)**
```json
{
  "event": "mouse_move_rel",
  "device_id": "1:2",
  "dx": 5,
  "dy": -3,
  "timestamp": 1699999999000
}
```

**Mouse Button Down**
```json
{
  "event": "mouse_button_down",
  "device_id": "1:2",
  "button": 1,
  "timestamp": 1699999999000
}
```

**Mouse Button Up**
```json
{
  "event": "mouse_button_up",
  "device_id": "1:2",
  "button": 1,
  "timestamp": 1699999999100
}
```

**Mouse Scroll**
```json
{
  "event": "mouse_scroll",
  "device_id": "1:2",
  "delta": 1,
  "timestamp": 1699999999000
}
```

### Client Acknowledgment

Clients must acknowledge processed events:

```json
{
  "event": "cmd",
  "cmd": "resume",
  "device_id": "1:2",
  "processed_count": 10
}
```

The `processed_count` field tells the streaming thread how many events have been processed, enabling backpressure management when the client's buffer fills up.

---

## Device Release

### Request Format

```json
{
  "event": "cmd",
  "cmd": "release_item",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid-2"
}
```

### Release Process

1. **Validate Request**
   - Check device_id is provided
   - Check correlation_id is provided
   - Check device exists in device_states

2. **Verify Ownership**
   - Check device_state->owner_pid matches client's pid
   - If not owner, return `NOT_OWNER` error

3. **Stop Streaming Thread**
   - Call `streaming_thread->stop()`
   - Remove from streaming_threads map

4. **Release USB Interface**
   - Call `libusb_release_interface(handle, interface_number)`

5. **Reattach Kernel Driver** (if it was detached)
   - If kernel_driver_attached flag is true
   - Call `libusb_attach_kernel_driver(handle, interface_number)`

6. **Close USB Handle**
   - Call `libusb_close(handle)`
   - Set handle to nullptr

7. **Clean Up State**
   - Remove from crash-recovery registry
   - Remove encryption state (if any)
   - Remove from device_states map

8. **Keep Device Descriptor**
   - Keep in available_devices for rediscovery
   - Reset handle to nullptr (will be reopened on next claim)

### Response: Success

```json
{
  "event": "item_released",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid-2",
  "status": "success"
}
```

### Response: Errors

**Device Not Claimed (101)**
```json
{
  "event": "item_released",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid-2",
  "error_code": 101,
  "msg": "Device not claimed: 1:2"
}
```

**Not Owner (102)**
```json
{
  "event": "item_released",
  "device_id": "1:2",
  "correlation_id": "client-generated-uuid-2",
  "error_code": 102,
  "msg": "Not owner of: 1:2"
}
```

---

## Hotplug Events

NoteUSB supports dynamic device attach/detach via libusb hotplug callbacks.

### Device Attached

When a USB device is plugged in:
1. libusb hotplug callback fires
2. NoteUSB builds device descriptor
3. Broadcasts to all clients:

```json
{
  "event": "device_attached",
  "device_id": "1:3",
  "item_info": {
    "device_id": "1:3",
    "vendor_id": 0x1234,
    "product_id": 0x5678,
    "manufacturer": "Example Corp",
    "product": "USB Keyboard",
    "serial_number": "ABC123",
    "bus_number": 1,
    "item_address": 3,
    "item_type": "HID"
  }
}
```

### Device Detached

When a USB device is unplugged:
1. libusb hotplug callback fires
2. Notifies all clients:

```json
{
  "event": "device_detached",
  "device_id": "1:2"
}
```

### Device Disconnected (Claimed Device)

When a claimed device is physically disconnected:
1. Streaming thread detects LIBUSB_ERROR_NO_DEVICE
2. Stops streaming
3. Releases interface and closes handle
4. Notifies client:

```json
{
  "event": "device_disconnected",
  "device_id": "1:2",
  "msg": "USB device physically disconnected"
}
```

Client options after disconnect:
1. Wait for device reattach (device infrastructure remains)
2. Release device with `release_item`

---

## Error Codes

| Code | Name | Description |
|------|------|-------------|
| 101 | ITEM_NOT_FOUND | Device doesn't exist or was removed |
| 102 | ITEM_NOT_AVAILABLE | Device already claimed by another client |
| 103 | PERMISSION_DENIED | Cannot open device (permission issue) |
| 104 | INTERFACE_IN_USE | Interface already claimed |
| 105 | NOT_OWNER | Cannot release device owned by another client |
| 106 | ALREADY_RELEASED | Device already released |

---

## Capabilities

NoteUSB exposes the following capabilities via the capability registry:

| Capability | Bit | Description |
|------------|-----|-------------|
| KEYBOARD | 0 | Keyboard input support |
| MOUSE | 1 | Mouse input support |
| RAW_MODE | 8 | Raw HID report mode |
| PARSED_MODE | 9 | Parsed/keyboard-mouse events |
| PASSTHROUGH_MODE | 10 | Passthrough mode |

These can be queried via `get_capabilities` command.

---

## Permissions

NoteUSB requires:
1. Access to `/dev/bus/usb/` for USB device enumeration
2. Access to USB devices (udev rules)
3. Permissions to detach kernel drivers (usually root or input group)

The daemon should run as root or have appropriate udev rules to access USB devices.

---

## See Also

- [NoteDaemon Device Acquisition API](../NoteDaemon/docs/device_acquisition_api.md)
- [Architecture](architecture.md)
- [Streaming Thread Design](streaming_thread_design.md)
- [Encryption and Hotplug Progress](encryption_hotplug_progress.md)