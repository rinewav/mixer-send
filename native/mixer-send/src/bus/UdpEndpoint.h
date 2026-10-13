// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

namespace mixersend
{

/*  One UDP socket CONNECTED to 127.0.0.1:<companion port>, plus a wake pipe.

    Connecting (rather than sendto on an unconnected socket) is deliberate:

    - the kernel then delivers only datagrams whose source is exactly
      127.0.0.1:<companion port>; anything else on the machine -- or on the
      LAN -- never reaches the plugin's parser;
    - the local address is 127.0.0.1:<ephemeral>, never a wildcard;
    - it needs no explicit bind, so it works inside a sandboxed AU host that
      grants only `network.client` (outbound). An explicit bind to
      127.0.0.1:0 is attempted first and its failure is ignored.

    A connected UDP socket reports "port unreachable" (ECONNREFUSED) on the
    next call after sending to a companion that is not running. That is
    swallowed: absence is the normal state, and nothing is ever queued.

    Everything here is non-blocking. Only the bus thread uses the socket;
    wake() may be called from any thread.

    POSIX: poll() on the socket and a self-pipe. Windows: Winsock, an event
    object bound to the socket with WSAEventSelect(FD_READ) and a second
    auto-reset event for wake(), waited on together; "port unreachable"
    arrives there as WSAECONNRESET and is swallowed the same way. */
class UdpEndpoint
{
public:
    UdpEndpoint() = default;
    ~UdpEndpoint();

    UdpEndpoint (const UdpEndpoint&) = delete;
    UdpEndpoint& operator= (const UdpEndpoint&) = delete;

    /** Creates the socket and the wake pipe. False if either failed (the bus
        then runs without a network: the plugin still works). */
    bool open (std::uint16_t companionPort) noexcept;
    void close() noexcept;

    bool isOpen() const noexcept { return socketFd != invalidSocket; }

    /** Sends one datagram; failures are counted, never reported. */
    bool send (const std::uint8_t* data, std::size_t size) noexcept;

    /** Receives one datagram if one is waiting. Returns its size, or -1 if
        none is waiting (errors, including a pending ECONNREFUSED, are
        cleared and also return -1). */
    long receive (std::uint8_t* buffer, std::size_t capacity) noexcept;

    /** Waits until a datagram is readable, wake() was called, or `timeoutMs`
        elapsed. Returns true if the socket is readable. */
    bool wait (int timeoutMs) noexcept;

    /** Interrupts wait() from any thread. */
    void wake() noexcept;

    std::uint16_t localPort() const noexcept;

    std::uint64_t sendFailures() const noexcept { return failedSends; }

    /** True if the last failed send was refused as too large (EMSGSIZE). */
    bool lastSendWasTooLarge() const noexcept { return tooLarge; }

private:
   #if defined(_WIN32)
    using NativeSocket = std::uintptr_t; // SOCKET, without pulling winsock2.h into every includer
    static constexpr NativeSocket invalidSocket = ~NativeSocket { 0 };
    void* socketEvent = nullptr; // WSAEVENT
    void* wakeEvent = nullptr;   // HANDLE
    bool winsockStarted = false;
   #else
    using NativeSocket = int;
    static constexpr NativeSocket invalidSocket = -1;
    int wakeRead = -1;
    int wakeWrite = -1;
   #endif
    NativeSocket socketFd = invalidSocket;
    std::uint64_t failedSends = 0;
    bool tooLarge = false;
};

} // namespace mixersend
