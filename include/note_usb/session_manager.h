// include/note_usb/session_manager.h
// Module-private session management for NoteUSB
// Handles hotplug broadcasting and session lifecycle

#ifndef NOTE_USB_SESSION_MANAGER_H
#define NOTE_USB_SESSION_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <syslog.h>
#include "notebytes.h"

namespace NoteUSB {

// Forward declaration
class NoteUSBSession;

/**
 * Session ID - unique identifier for a client connection
 */
using SessionID = int;  // Client file descriptor

/**
 * Session ID - unique identifier for a client connection
 */
struct Session {
    SessionID client_fd;
    pid_t client_pid;
    bool is_active;
    NoteUSBSession* session;

    Session(SessionID fd, pid_t pid, NoteUSBSession* sess)
        : client_fd(fd), client_pid(pid), is_active(true), session(sess) {}
};

/**
 * Session Manager - module-private session registry
 * Manages all active sessions for hotplug notifications
 * 
 * This is module-private - no other modules should access it.
 */
class SessionManager {
public:
    static SessionManager& instance() {
        static SessionManager manager;
        return manager;
    }

    // ===== Session Management =====

    /**
     * Register a session for hotplug notifications
     */
    void register_session(SessionID client_fd, pid_t client_pid, NoteUSBSession* session) {
        std::lock_guard<std::mutex> lock(mutex_);

        SessionID sid = next_session_id_++;
        sessions_[sid] = std::make_unique<Session>(client_fd, client_pid, session);

        syslog(LOG_INFO, "Session registered for hotplug notifications (total: %zu, sid=%d)",
               sessions_.size(), sid);
    }

    /**
     * Unregister a session (called on disconnect)
     */
    void unregister_session(SessionID session_id) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            sessions_.erase(it);
            syslog(LOG_INFO, "Session unregistered (total: %zu, sid=%d)",
                   sessions_.size(), session_id);
        }
    }

    /**
     * Unregister a session by client FD and PID
     */
    void unregister_session_by_client(SessionID client_fd, pid_t client_pid) {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto& [sid, session] : sessions_) {
            if (session->client_fd == client_fd && session->client_pid == client_pid) {
                sessions_.erase(sid);
                syslog(LOG_INFO, "Session unregistered by client (total: %zu, sid=%d)",
                       sessions_.size(), sid);
                break;
            }
        }
    }

    /**
     * Send a message to all active sessions
     * Used for hotplug notifications (device attach/detach)
     */
    void broadcast_to_all_sessions(const NoteBytes::Object& msg) {
        std::lock_guard<std::mutex> lock(mutex_);

        int sent_count = 0;
        for (const auto& [sid, session] : sessions_) {
            if (session && session->is_active && session->client_fd >= 0) {
                try {
                    send_message(session->client_fd, msg);
                    sent_count++;
                } catch (const std::exception& e) {
                    syslog(LOG_WARNING, "Failed to broadcast to session fd=%d: %s",
                           session->client_fd, e.what());
                    // Mark session as inactive (will be cleaned up)
                    session->is_active = false;
                }
            }
        }

        syslog(LOG_DEBUG, "Broadcasted message to %zu sessions", sent_count);
    }

    /**
     * Get total number of active sessions
     */
    size_t session_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

    /**
     * Check if a session is active
     */
    bool is_session_active(SessionID client_fd) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [sid, session] : sessions_) {
            if (session && session->is_active && session->client_fd == client_fd) {
                return true;
            }
        }
        return false;
    }

private:
    SessionManager() = default;
    ~SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    /**
     * Send a message to a specific session
     */
    void send_message(SessionID client_fd, const NoteBytes::Object& msg) {
        // Serialize message to bytes
        std::vector<uint8_t> data = msg.serialize_with_header();
        
        // Write to socket
        ssize_t sent = write(client_fd, data.data(), data.size());
        if (sent == -1) {
            syslog(LOG_ERR, "SessionManager: failed to send message to fd=%d: %s",
                   client_fd, strerror(errno));
        }
    }

    std::mutex mutex_;
    std::map<SessionID, std::unique_ptr<Session>> sessions_;
    SessionID next_session_id_ = 1;  // Start at 1 to distinguish from invalid FD=0
};

} // namespace NoteUSB

#endif // NOTE_USB_SESSION_MANAGER_H
