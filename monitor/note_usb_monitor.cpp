// monitor/note_usb_monitor.cpp
// Device monitor for NoteUSB - reattaches kernel drivers on daemon exit
//
// Usage: note_usb_monitor <pid>
//
// On termination of the watched PID, the monitor:
// 1. Reads the device registry file
// 2. Finds all devices claimed by the watched PID  
// 3. Reattaches the kernel driver for each device
// 4. Removes the entries from the registry
// 5. Exits

#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <string>
#include <vector>
#include <cstring>

// Include device registry from NoteDaemon
#include "device_registry.h"

/**
 * Reattach kernel driver for a single device.
 */
static bool reattach_kernel_driver(const ClaimedDevice& device) {
    libusb_context* ctx = nullptr;
    int rc = libusb_init(&ctx);
    if (rc != LIBUSB_SUCCESS) {
        syslog(LOG_ERR, "NoteUSB Monitor: Failed to init libusb: %s",
               libusb_error_name(rc));
        return false;
    }
    
    libusb_device** list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx, &list);
    if (cnt < 0) {
        syslog(LOG_ERR, "NoteUSB Monitor: Failed to get device list");
        libusb_exit(ctx);
        return false;
    }
    
    // Find the device by bus:address
    libusb_device* target_device = nullptr;
    for (ssize_t i = 0; i < cnt; i++) {
        uint8_t bus = libusb_get_bus_number(list[i]);
        uint8_t address = libusb_get_device_address(list[i]);
        std::string id = std::to_string(bus) + ":" + std::to_string(address);
        
        if (id == device.device_id) {
            target_device = list[i];
            break;
        }
    }
    
    if (!target_device) {
        syslog(LOG_WARNING, "NoteUSB Monitor: Device %s not found",
               device.device_id.c_str());
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return false;
    }
    
    libusb_device_handle* handle = nullptr;
    rc = libusb_open(target_device, &handle);
    if (rc != LIBUSB_SUCCESS) {
        syslog(LOG_WARNING, "NoteUSB Monitor: Cannot open device %s: %s",
               device.device_id.c_str(), libusb_error_name(rc));
        libusb_free_device_list(list, 1);
        libusb_exit(ctx);
        return false;
    }
    
    rc = libusb_attach_kernel_driver(handle, device.interface_number);
    if (rc == LIBUSB_SUCCESS) {
        syslog(LOG_INFO, "NoteUSB Monitor: Reattached kernel driver for device %s",
               device.device_id.c_str());
    } else if (rc == LIBUSB_ERROR_NOT_FOUND) {
        syslog(LOG_DEBUG, "NoteUSB Monitor: No kernel driver to reattach for device %s",
               device.device_id.c_str());
    } else {
        syslog(LOG_WARNING, "NoteUSB Monitor: Failed to reattach for device %s: %s",
               device.device_id.c_str(), libusb_error_name(rc));
    }
    
    libusb_close(handle);
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    
    return (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_NOT_FOUND);
}

/**
 * Main monitor - watches PID and cleans up orphaned devices
 */
int monitor_main(pid_t watched_pid) {
    setsid();
    
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    
    syslog(LOG_INFO, "NoteUSB Monitor started — watching PID %d", watched_pid);
    
    while (true) {
        if (kill(watched_pid, 0) != 0) {
            syslog(LOG_INFO, "NoteUSB Monitor: PID %d terminated, cleaning up",
                   watched_pid);
            
            auto devices = DeviceRegistry::get_all_devices();
            
            int reattached = 0;
            int failed = 0;
            
            for (const auto& device : devices) {
                if (device.pid == watched_pid) {
                    if (reattach_kernel_driver(device)) {
                        reattached++;
                    } else {
                        failed++;
                    }
                }
            }
            
            DeviceRegistry::remove_all_by_pid(watched_pid);
            
            syslog(LOG_INFO, "NoteUSB Monitor: Done — %d reattached, %d failed",
                   reattached, failed);
            
            break;
        }
        
        usleep(200000);  // 200ms
    }
    
    syslog(LOG_INFO, "NoteUSB Monitor exiting");
    return 0;
}

/**
 * Main entry point
 */
int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <pid>\n", argv[0]);
        return 1;
    }
    
    pid_t watched_pid = std::atoi(argv[1]);
    if (watched_pid <= 0) {
        fprintf(stderr, "Invalid PID: %s\n", argv[1]);
        return 1;
    }
    
    openlog("note_usb-monitor", LOG_PID, LOG_DAEMON);
    int result = monitor_main(watched_pid);
    closelog();
    
    return result;
}