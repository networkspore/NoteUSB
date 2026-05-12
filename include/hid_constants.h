// include/hid_constants.h
// Shared HID constants — replaces magic numbers throughout the codebase

#ifndef HID_CONSTANTS_H
#define HID_CONSTANTS_H

#include <cstdint>
#include <sys/time.h>

namespace HidConstants {
    static constexpr uint8_t kDefaultEndpointIn = 0x81;
    static constexpr size_t kHidReportBufferSize = 64;
    static constexpr size_t kHidReportSize = 8;
    static constexpr suseconds_t kLibusbPollTimeoutUs = 1000;
    static constexpr size_t kSpscQueueCapacity = 1024;
    static constexpr size_t kMaxClientEventQueue = 1000;
}

#endif // HID_CONSTANTS_H
