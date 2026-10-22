#pragma once
// Copied from zx-spectrum-emulator (cpp-core/src/net.h) and left to drift.
// Minimal blocking TCP listener/socket wrapper.
//
// Thread-per-connection rather than an async runtime: at this scale (a
// debugger, an MCP client and a screen viewer) that is a handful of threads
// spending nearly all their time blocked on a socket, and it keeps the
// protocol code straight-line and readable.

#include <atomic>
#include <cstdint>
#include <string>

namespace ql::net {

/// Initialises Winsock. Safe to call more than once.
bool startup(std::string& error);

/// A connected socket. Moves, does not copy; closes on destruction.
class Socket {
public:
    Socket() = default;
    explicit Socket(uintptr_t handle) : handle_(handle) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const;
    void close();

    /// Reads up to `len` bytes. Returns bytes read, 0 on clean close, <0 on
    /// error.
    int recv(char* buf, int len);
    /// Writes all of `data`. False if the peer went away.
    bool send_all(const char* data, size_t len);
    bool send_all(const std::string& s) { return send_all(s.data(), s.size()); }

private:
    uintptr_t handle_ = ~uintptr_t(0);
};

/// A listening socket.
class Listener {
public:
    Listener() = default;
    ~Listener();
    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    /// Binds and listens. Returns false and fills `error` on failure. Port 0
    /// asks the system for any free port, and port() then says which.
    bool listen(const std::string& host, uint16_t port, std::string& error);
    /// The port actually bound, or 0 when not listening.
    uint16_t port() const;
    bool valid() const;
    /// Blocks for the next connection. Returns an invalid Socket on failure.
    Socket accept();
    /// Closes it; an accept blocked on another thread then fails, and
    /// valid() is false, so a serving loop can tell it has been stopped.
    void close();

private:
    /// Atomic because close() may come from another thread than accept().
    std::atomic<uintptr_t> handle_{~uintptr_t(0)};
};

/// Connects to a listener, for tests and clients. Returns an invalid Socket
/// and fills `error` on failure.
Socket connect(const std::string& host, uint16_t port, std::string& error);

} // namespace ql::net
