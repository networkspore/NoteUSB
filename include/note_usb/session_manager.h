// include/note_usb/session_manager.h
// Module-private session registry for NoteUSB.
//
// Two-socket model:
//   • management_fd  – the client's management socket connection.  Used to
//     send ITEM_CLAIMED, ITEM_RELEASED, ITEM_LIST, hotplug events, and errors
//     back to the Java IODaemon.
//   • device_fd      – the separate socket the Java client opens after it
//     receives ITEM_CLAIMED.  Owned exclusively by NoteUSBSession for streaming
//     raw/parsed HID data.
//
// SessionManager stores sessions keyed by client PID and provides:
//   • Broadcast to all management fds (hotplug: DEVICE_ATTACHED/DETACHED)
//   • Per-device management-fd lookup (ITEM_CLAIMED / ITEM_RELEASED responses)

#ifndef NOTE_USB_SESSION_MANAGER_H
#define NOTE_USB_SESSION_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <syslog.h>
#include <unistd.h>
#include <cstring>
#include "notebytes.h"

namespace NoteUSB {

class NoteUSBSession;

/**
 * Per-session record held by the SessionManager.
 */
struct SessionEntry {
    pid_t client_pid;
    int   management_fd;  // management socket – write hotplug/response events here
    bool  is_active;
    NoteUSBSession* session;

    SessionEntry(pid_t pid, int mgmt_fd, NoteUSBSession* sess)
        : client_pid(pid)
        , management_fd(mgmt_fd)
        , is_active(true)
        , session(sess)
    {}
};

/**
 * SessionManager – module-private singleton.
 *
 * Tracks all connected clients.  Provides:
 *  • register_session() / unregister_session()
 *  • broadcast_to_all()   – send a message to every active management fd
 *  • send_to_session()    – send to one management fd by PID
 */
class SessionManager {
public:
    static SessionManager& instance() {
        static SessionManager inst;
        return inst;
    }

    // ── Session lifecycle ──────────────────────────────────────────────────────

    /**
     * Register a newly-connected client.
     * @param client_pid    Client process ID (session key).
     * @param management_fd Management socket fd for this client.
     * @param session       Pointer to the owning NoteUSBSession (may be nullptr
     *                      initially; updated via update_session()).
     */
    void register_session(pid_t client_pid, int management_fd,
                          NoteUSBSession* session) {
        std::lock_guard lock(mutex_);
        sessions_.emplace(client_pid,
                          std::make_unique<SessionEntry>(client_pid,
                                                        management_fd,
                                                        session));
        syslog(LOG_INFO,
               "[SessionManager] registered pid=%d fd=%d (total=%zu)",
               client_pid, management_fd, sessions_.size());
    }

    /**
     * Update the NoteUSBSession pointer for an already-registered pid.
     * Called once the session object is fully constructed.
     */
    void update_session(pid_t client_pid, NoteUSBSession* session) {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(client_pid);
        if (it != sessions_.end()) {
            it->second->session = session;
        }
    }

    /**
     * Unregister a session.  Management fd is NOT closed here; the caller
     * (NoteUSBModule) is responsible for closing it after all responses are sent.
     */
    void unregister_session(pid_t client_pid) {
        std::lock_guard lock(mutex_);
        auto erased = sessions_.erase(client_pid);
        if (erased) {
            syslog(LOG_INFO,
                   "[SessionManager] unregistered pid=%d (total=%zu)",
                   client_pid, sessions_.size());
        }
    }

    // ── Sending ───────────────────────────────────────────────────────────────

    /**
     * Broadcast a message to all active sessions' management fds.
     * Used for hotplug notifications (DEVICE_ATTACHED / DEVICE_DETACHED).
     * Failed writes mark the session inactive; stale entries are pruned lazily.
     */
    void broadcast_to_all(const NoteBytes::Object& msg) {
        std::lock_guard lock(mutex_);
        std::vector<uint8_t> data = msg.serialize_with_header();

        for (auto& [pid, entry] : sessions_) {
            if (!entry->is_active) continue;
            if (!write_bytes(entry->management_fd, data)) {
                syslog(LOG_WARNING,
                       "[SessionManager] broadcast failed to pid=%d, marking inactive",
                       pid);
                entry->is_active = false;
            }
        }
    }

    /**
     * Send a message to a specific session's management fd.
     * Returns false if the session is not found or the write fails.
     */
    bool send_to_session(pid_t client_pid, const NoteBytes::Object& msg) {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(client_pid);
        if (it == sessions_.end() || !it->second->is_active) {
            return false;
        }

        std::vector<uint8_t> data = msg.serialize_with_header();
        bool ok = write_bytes(it->second->management_fd, data);
        if (!ok) {
            syslog(LOG_WARNING,
                   "[SessionManager] send failed to pid=%d, marking inactive", client_pid);
            it->second->is_active = false;
        }
        return ok;
    }

    // ── Query ─────────────────────────────────────────────────────────────────

    size_t session_count() const {
        std::lock_guard lock(mutex_);
        return sessions_.size();
    }

    bool is_active(pid_t client_pid) const {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(client_pid);
        return it != sessions_.end() && it->second->is_active;
    }

    /** Return the management fd for a PID, or -1 if not found. */
    int get_management_fd(pid_t client_pid) const {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(client_pid);
        return (it != sessions_.end()) ? it->second->management_fd : -1;
    }

private:
    SessionManager()  = default;
    ~SessionManager() = default;
    SessionManager(const SessionManager&)            = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /** Write a fully-framed byte buffer to a fd. Returns false on error. */
    static bool write_bytes(int fd, const std::vector<uint8_t>& data) {
        ssize_t sent = ::write(fd, data.data(), data.size());
        if (sent < 0) {
            syslog(LOG_ERR, "[SessionManager] write fd=%d: %s",
                   fd, strerror(errno));
            return false;
        }
        return true;
    }

    mutable std::mutex mutex_;
    std::map<pid_t, std::unique_ptr<SessionEntry>> sessions_;
};

} // namespace NoteUSB

#endif // NOTE_USB_SESSION_MANAGER_H