// src/device_handler.cpp
// DeviceHandler implementation for NoteUSB.
//
// Two-socket changes vs. old version:
//   • claim_device() / release_device() accept reply_fd and write responses directly.
//   • claim_device() calls ownership_registry_->register_device() on success.
//   • release_device() calls ownership_registry_->unregister_device() on success.
//   • send_device_list() writes ITEM_LIST to reply_fd (or broadcasts if -1).
//   • All response formatting is done here; the module layer is no longer
//     responsible for building ITEM_CLAIMED / ITEM_RELEASED objects.

#include "note_usb/device_handler.h"
#include "note_usb/session_manager.h"

#include <syslog.h>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <fstream>
#include <unistd.h>

#include "note_messaging.h"
#include "notebytes.h"
#include "notebytes_writer.h"
#include "event_bytes.h"
#include "capability_registry.h"

namespace NoteUSB {

// =============================================================================
// USBDeviceDescriptor
// =============================================================================

NoteBytes::Object USBDeviceDescriptor::to_notebytes() const {
    NoteBytes::Object obj;
    obj.add(NoteBytes::Value("device_id"),          NoteBytes::Value(device_id));
    obj.add(NoteBytes::Value("vendor_id"),          NoteBytes::Value((int)vendor_id));
    obj.add(NoteBytes::Value("product_id"),         NoteBytes::Value((int)product_id));
    obj.add(NoteBytes::Value("interface_number"),   NoteBytes::Value(interface_number));
    obj.add(NoteBytes::Value("kernel_driver_attached"),
            NoteBytes::Value(kernel_driver_attached));
    return obj;
}

// =============================================================================
// DeviceRegistry
// =============================================================================

const std::string& DeviceRegistry::path() {
    static std::string p = "/run/netnotes/modules/note_usb/device_registry.json";
    return p;
}

std::string& DeviceRegistry::registry_path() {
    static std::string p = "/run/netnotes/modules/note_usb/device_registry.json";
    return p;
}

std::mutex& DeviceRegistry::registry_mutex() {
    static std::mutex m;
    return m;
}

void DeviceRegistry::add_device(const std::string& device_id,
                                int interface_number,
                                bool kernel_driver_attached) {
    std::lock_guard lock(registry_mutex());

    ClaimedDevice d;
    d.pid                    = getpid();
    d.device_id              = device_id;
    d.interface_number       = interface_number;
    d.kernel_driver_attached = kernel_driver_attached;

    auto devices = read_registry();
    devices.erase(std::remove_if(devices.begin(), devices.end(),
        [&](const ClaimedDevice& x){ return x.device_id == device_id; }),
        devices.end());
    devices.push_back(d);
    write_registry(devices);

    syslog(LOG_INFO, "[DeviceRegistry] added %s (iface=%d)",
           device_id.c_str(), interface_number);
}

bool DeviceRegistry::remove_device(const std::string& device_id) {
    std::lock_guard lock(registry_mutex());

    auto devices = read_registry();
    auto it = std::remove_if(devices.begin(), devices.end(),
        [&](const ClaimedDevice& x){ return x.device_id == device_id; });

    if (it == devices.end()) {
        syslog(LOG_WARNING, "[DeviceRegistry] %s not found", device_id.c_str());
        return false;
    }
    devices.erase(it, devices.end());
    write_registry(devices);
    syslog(LOG_INFO, "[DeviceRegistry] removed %s", device_id.c_str());
    return true;
}

std::vector<ClaimedDevice> DeviceRegistry::get_all_devices() {
    std::lock_guard lock(registry_mutex());
    return read_registry();
}

void DeviceRegistry::remove_all() {
    std::lock_guard lock(registry_mutex());
    write_registry({});
    syslog(LOG_INFO, "[DeviceRegistry] cleared");
}

// ── JSON serialisation (no external deps) ────────────────────────────────────

std::string DeviceRegistry::json_object(const ClaimedDevice& d) {
    std::ostringstream oss;
    oss << "{"
        << "\"pid\":"                    << d.pid                    << ","
        << "\"device_id\":\""            << d.device_id              << "\","
        << "\"interface_number\":"       << d.interface_number       << ","
        << "\"kernel_driver_attached\":" << (d.kernel_driver_attached ? "true" : "false")
        << "}";
    return oss.str();
}

bool DeviceRegistry::write_registry(const std::vector<ClaimedDevice>& devices) {
    // Ensure directory exists
    std::string dir = registry_path();
    if (auto slash = dir.find_last_of('/'); slash != std::string::npos) {
        mkdir(dir.substr(0, slash).c_str(), 0755);
    }

    std::ofstream f(registry_path());
    if (!f) {
        syslog(LOG_WARNING, "[DeviceRegistry] cannot write %s",
               registry_path().c_str());
        return false;
    }
    f << "[";
    for (size_t i = 0; i < devices.size(); ++i) {
        if (i) f << ",";
        f << json_object(devices[i]);
    }
    f << "]\n";
    return true;
}

std::vector<ClaimedDevice> DeviceRegistry::read_registry() {
    std::vector<ClaimedDevice> devices;

    std::ifstream f(registry_path());
    if (!f) return devices;   // file doesn't exist yet

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Simple brace-matched JSON array parser
    size_t pos = 0;
    while (pos < content.size()) {
        size_t obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;

        int depth = 0;
        size_t obj_end = obj_start;
        for (size_t i = obj_start; i < content.size(); ++i) {
            if (content[i] == '{') ++depth;
            if (content[i] == '}') {
                if (--depth == 0) { obj_end = i; break; }
            }
        }
        if (obj_end == obj_start) { ++pos; continue; }

        std::string obj = content.substr(obj_start + 1, obj_end - obj_start - 1);
        ClaimedDevice d;

        // Parse known keys
        auto extract_string = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\":\"";
            auto p = obj.find(search);
            if (p == std::string::npos) return {};
            p += search.size();
            auto e = obj.find('"', p);
            return (e == std::string::npos) ? std::string{} : obj.substr(p, e - p);
        };
        auto extract_int = [&](const std::string& key) -> long long {
            std::string search = "\"" + key + "\":";
            auto p = obj.find(search);
            if (p == std::string::npos) return 0;
            p += search.size();
            try { return std::stoll(obj.substr(p)); } catch (...) { return 0; }
        };
        auto extract_bool = [&](const std::string& key) -> bool {
            std::string search = "\"" + key + "\":";
            auto p = obj.find(search);
            if (p == std::string::npos) return false;
            p += search.size();
            while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t')) ++p;
            return obj.substr(p, 4) == "true";
        };

        d.pid                    = (pid_t)extract_int("pid");
        d.device_id              = extract_string("device_id");
        d.interface_number       = (int)extract_int("interface_number");
        d.kernel_driver_attached = extract_bool("kernel_driver_attached");

        if (!d.device_id.empty()) devices.push_back(d);
        pos = obj_end + 1;
    }

    return devices;
}

// =============================================================================
// DeviceHandler
// =============================================================================

DeviceHandler::DeviceHandler(NoteDaemon::DeviceOwnershipRegistry* ownership_registry,
                             std::string_view module_name)
    : ownership_registry_(ownership_registry)
    , module_name_(module_name)
{
    int rc = libusb_init(&ctx_);
    if (rc != 0) {
        syslog(LOG_ERR, "[DeviceHandler] libusb_init failed: %s",
               libusb_error_name(rc));
    }
}

DeviceHandler::~DeviceHandler() {
    release_all_devices();
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

// ── Discovery ─────────────────────────────────────────────────────────────────

void DeviceHandler::start_discovery() {
    if (running_) return;
    running_ = true;
    syslog(LOG_INFO, "[DeviceHandler] discovery started");
    discover_devices();
}

void DeviceHandler::stop_discovery() {
    running_ = false;
    syslog(LOG_INFO, "[DeviceHandler] discovery stopped");
}

void DeviceHandler::discover_devices() {
    if (!ctx_) return;

    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    if (count < 0) {
        syslog(LOG_ERR, "[DeviceHandler] libusb_get_device_list: %s",
               libusb_error_name((int)count));
        return;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* dev = list[i];
        if (!is_hid_device(dev)) continue;

        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) continue;

        uint8_t bus  = libusb_get_bus_number(dev);
        uint8_t addr = libusb_get_device_address(dev);
        std::string device_id = std::to_string(bus) + ":" + std::to_string(addr);

        if (available_devices_.count(device_id)) continue; // already known

        auto d = std::make_shared<USBDeviceDescriptor>();
        d->device_id         = device_id;
        d->vendor_id         = desc.idVendor;
        d->product_id        = desc.idProduct;
        d->interface_number  = 0;
        d->handle            = nullptr;

        available_devices_[device_id] = d;
        syslog(LOG_INFO, "[DeviceHandler] discovered HID %s (VID:PID %04x:%04x)",
               device_id.c_str(), desc.idVendor, desc.idProduct);
    }

    libusb_free_device_list(list, 1);
    syslog(LOG_INFO, "[DeviceHandler] discovery complete – %zu HID device(s)",
           available_devices_.size());
}

// ── Device list ───────────────────────────────────────────────────────────────

void DeviceHandler::send_device_list(int reply_fd) {
    // Re-scan before responding
    discover_devices();

    NoteBytes::Object response;
    response.add(NoteMessaging::Keys::EVENT,
                 NoteMessaging::ProtocolMessages::ITEM_LIST);

    NoteBytes::Array arr;
    for (const auto& [id, dev] : available_devices_) {
        auto obj    = dev->to_notebytes();
        auto bytes  = obj.serialize();
        arr.add(NoteBytes::Value(bytes, NoteBytes::Type::OBJECT));
    }
    response.add(NoteMessaging::Keys::ITEMS, arr.as_value());

    if (reply_fd >= 0) {
        write_to_fd(reply_fd, response);
    } else {
        // No specific fd – broadcast to all connected sessions
        SessionManager::instance().broadcast_to_all(response);
    }

    syslog(LOG_INFO, "[DeviceHandler] sent device list (%zu devices)",
           available_devices_.size());
}

// ── Claim ─────────────────────────────────────────────────────────────────────

NoteDaemon::Error DeviceHandler::claim_device(const NoteBytes::Object& msg,
                                               int reply_fd,
                                               pid_t client_pid) {
    auto* dev_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
    if (!dev_id_val) {
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::INVALID_MESSAGE, "Missing DEVICE_ID");
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            "", err.code, std::string(err.message()));
        return err;
    }

    std::string device_id = dev_id_val->as_string();
    syslog(LOG_INFO, "[DeviceHandler] claim request: device=%s pid=%d",
           device_id.c_str(), client_pid);

    // Validate device exists
    auto dev_it = available_devices_.find(device_id);
    if (dev_it == available_devices_.end()) {
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
            "Device not found: " + device_id);
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    // Already claimed?
    if (claimed_devices_.count(device_id)) {
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::ITEM_NOT_AVAILABLE,
            "Device already claimed: " + device_id);
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    // ── Open the libusb device ────────────────────────────────────────────────

    // Find libusb_device for this id
    libusb_device** list = nullptr;
    ssize_t count = libusb_get_device_list(ctx_, &list);
    libusb_device* usb_dev = nullptr;

    for (ssize_t i = 0; i < count; ++i) {
        uint8_t bus  = libusb_get_bus_number(list[i]);
        uint8_t addr = libusb_get_device_address(list[i]);
        if (std::to_string(bus) + ":" + std::to_string(addr) == device_id) {
            usb_dev = list[i];
            break;
        }
    }

    if (!usb_dev) {
        libusb_free_device_list(list, 1);
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
            "USB device disappeared: " + device_id);
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    libusb_device_handle* handle = nullptr;
    int rc = libusb_open(usb_dev, &handle);
    libusb_free_device_list(list, 1);

    if (rc != LIBUSB_SUCCESS) {
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::PERMISSION_DENIED,
            std::string("Cannot open ") + device_id + ": " + libusb_error_name(rc));
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    auto dev_desc = dev_it->second;
    int iface     = dev_desc->interface_number;

    rc = libusb_claim_interface(handle, iface);
    if (rc != LIBUSB_SUCCESS) {
        libusb_close(handle);
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::PERMISSION_DENIED,
            std::string("Cannot claim interface for ") + device_id + ": " +
            libusb_error_name(rc));
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_CLAIMED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    // Detach kernel driver if active
    bool kdriver = false;
    if (libusb_kernel_driver_active(handle, iface) == 1) {
        if (libusb_detach_kernel_driver(handle, iface) == LIBUSB_SUCCESS) {
            kdriver = true;
            syslog(LOG_INFO, "[DeviceHandler] detached kernel driver for %s",
                   device_id.c_str());
        }
    }

    // Store state
    DeviceInfo info;
    info.device_id              = device_id;
    info.handle                 = handle;
    info.interface_number       = iface;
    info.kernel_driver_attached = kdriver;
    info.capabilities           = Capabilities::Masks::mode_mask();
    info.owner_pid              = client_pid;

    claimed_devices_[device_id] = info;
    dev_desc->handle             = handle;
    dev_desc->kernel_driver_attached = kdriver;
    dev_desc->owner_pid          = client_pid;

    // Persist to crash-recovery registry
    DeviceRegistry::add_device(device_id, iface, kdriver);

    // Register ownership so the core can route the DEVICE_HANDSHAKE
    if (ownership_registry_) {
        ownership_registry_->register_device(device_id, module_name_);
    }

    // Send ITEM_CLAIMED on management socket
    NoteBytes::Object claimed;
    claimed.add(NoteMessaging::Keys::EVENT,
                NoteMessaging::ProtocolMessages::ITEM_CLAIMED);
    claimed.add(NoteMessaging::Keys::DEVICE_ID,  device_id);
    claimed.add(NoteMessaging::Keys::STATUS,     std::string("claimed"));
    claimed.add(NoteMessaging::Keys::MODULE_ID,  module_name_);
    write_to_fd(reply_fd, claimed);

    syslog(LOG_INFO, "[DeviceHandler] claimed device=%s for pid=%d",
           device_id.c_str(), client_pid);

    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

// ── Release ───────────────────────────────────────────────────────────────────

NoteDaemon::Error DeviceHandler::release_device(const NoteBytes::Object& msg,
                                                  int reply_fd) {
    auto* dev_id_val = msg.get(NoteMessaging::Keys::DEVICE_ID);
    if (!dev_id_val) {
        return NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::INVALID_MESSAGE, "Missing DEVICE_ID");
    }

    std::string device_id = dev_id_val->as_string();
    syslog(LOG_INFO, "[DeviceHandler] release request: device=%s", device_id.c_str());

    auto it = claimed_devices_.find(device_id);
    if (it == claimed_devices_.end()) {
        auto err = NoteDaemon::Error::from_code(
            NoteMessaging::ErrorCodes::DEVICE_NOT_FOUND,
            "Not claimed: " + device_id);
        send_error_response(reply_fd,
                            NoteMessaging::ProtocolMessages::ITEM_RELEASED,
                            device_id, err.code, std::string(err.message()));
        return err;
    }

    auto& info = it->second;
    if (info.handle) {
        libusb_release_interface(info.handle, info.interface_number);
        if (info.kernel_driver_attached) {
            if (libusb_attach_kernel_driver(info.handle, info.interface_number)
                    == LIBUSB_SUCCESS) {
                syslog(LOG_INFO, "[DeviceHandler] reattached kernel driver for %s",
                       device_id.c_str());
            }
        }
        libusb_close(info.handle);
    }

    claimed_devices_.erase(it);

    if (auto dev_it = available_devices_.find(device_id);
        dev_it != available_devices_.end()) {
        dev_it->second->handle                 = nullptr;
        dev_it->second->kernel_driver_attached = false;
        dev_it->second->owner_pid              = 0;
    }

    DeviceRegistry::remove_device(device_id);

    // Unregister ownership – the device socket will be closed by the client
    if (ownership_registry_) {
        ownership_registry_->unregister_device(device_id);
    }

    // Send ITEM_RELEASED on management socket
    NoteBytes::Object released;
    released.add(NoteMessaging::Keys::EVENT,
                 NoteMessaging::ProtocolMessages::ITEM_RELEASED);
    released.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    released.add(NoteMessaging::Keys::STATUS,
                 NoteMessaging::ProtocolMessages::SUCCESS);
    write_to_fd(reply_fd, released);

    syslog(LOG_INFO, "[DeviceHandler] released device=%s", device_id.c_str());
    return NoteDaemon::Error(NoteDaemon::ErrorCodes::SUCCESS, "");
}

// ── Bulk release (shutdown) ───────────────────────────────────────────────────

void DeviceHandler::release_all_devices() {
    for (auto& [id, info] : claimed_devices_) {
        if (!info.handle) continue;
        libusb_release_interface(info.handle, info.interface_number);
        if (info.kernel_driver_attached) {
            libusb_attach_kernel_driver(info.handle, info.interface_number);
        }
        libusb_close(info.handle);

        if (ownership_registry_) {
            ownership_registry_->unregister_device(id);
        }
    }
    claimed_devices_.clear();
    available_devices_.clear();
    DeviceRegistry::remove_all();
    syslog(LOG_INFO, "[DeviceHandler] released all devices");
}

void DeviceHandler::collect_errors(std::vector<NoteDaemon::Error>& /*errors*/) {
    // Reserved for future per-device error queues
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool DeviceHandler::is_hid_device(libusb_device* device) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(device, &cfg) != LIBUSB_SUCCESS)
        return false;

    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; ++i) {
        const auto& iface = cfg->interface[i];
        if (iface.num_altsetting > 0 &&
            iface.altsetting[0].bInterfaceClass == LIBUSB_CLASS_HID) {
            found = true;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

std::shared_ptr<USBDeviceDescriptor>
DeviceHandler::find_device_by_id(const std::string& id) {
    auto it = available_devices_.find(id);
    return (it != available_devices_.end()) ? it->second : nullptr;
}

void DeviceHandler::write_to_fd(int fd, const NoteBytes::Object& obj) {
    if (fd < 0) return;
    try {
        NoteBytes::Writer writer(fd, /*owns_fd=*/false);
        writer.write(obj);
        writer.flush();
    } catch (const std::exception& e) {
        syslog(LOG_WARNING, "[DeviceHandler] write_to_fd fd=%d: %s", fd, e.what());
    }
}

void DeviceHandler::send_error_response(int fd,
                                         const NoteBytes::Value& event,
                                         const std::string& device_id,
                                         int code,
                                         const std::string& message) {
    NoteBytes::Object err;
    err.add(NoteMessaging::Keys::EVENT,     event);
    err.add(NoteMessaging::Keys::ERROR,     code);
    err.add(NoteMessaging::Keys::MSG,       message);
    if (!device_id.empty()) {
        err.add(NoteMessaging::Keys::DEVICE_ID, device_id);
    }
    write_to_fd(fd, err);
}

} // namespace NoteUSB