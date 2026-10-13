// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

#include "bus/UdpEndpoint.h"

#if defined(_WIN32)

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#pragma comment (lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
 #define SIO_UDP_CONNRESET _WSAIOW (IOC_VENDOR, 12)
#endif

namespace mixersend
{

namespace
{
    sockaddr_in loopback (std::uint16_t port) noexcept
    {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons (port);
        address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        return address;
    }

    SOCKET native (std::uintptr_t s) noexcept { return static_cast<SOCKET> (s); }
}

UdpEndpoint::~UdpEndpoint()
{
    close();
}

bool UdpEndpoint::open (std::uint16_t companionPort) noexcept
{
    close();

    WSADATA data {};
    if (::WSAStartup (MAKEWORD (2, 2), &data) != 0)
        return false;
    winsockStarted = true;

    wakeEvent = ::CreateEventW (nullptr, FALSE, FALSE, nullptr); // auto-reset

    if (wakeEvent == nullptr)
        return false;

    // No handle inheritance: hosts spawn helper processes; the socket must not leak into them.
    const SOCKET s = ::WSASocketW (AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);

    if (s == INVALID_SOCKET)
        return false;

    socketFd = static_cast<std::uintptr_t> (s);

    // Windows turns an ICMP "port unreachable" (companion absent) into
    // WSAECONNRESET on the next recv. Absence is the normal state: ask Winsock
    // not to report it at all (receive() also swallows it, like ECONNREFUSED).
    BOOL reportConnReset = FALSE;
    DWORD returned = 0;
    ::WSAIoctl (s, SIO_UDP_CONNRESET, &reportConnReset, sizeof (reportConnReset), nullptr, 0, &returned, nullptr, nullptr);

    // Enough for a burst of control datagrams; the bus drains every wake.
    const int receiveBuffer = 64 * 1024;
    ::setsockopt (s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*> (&receiveBuffer), sizeof (receiveBuffer));

    auto local = loopback (0);
    ::bind (s, reinterpret_cast<const sockaddr*> (&local), sizeof (local)); // failure is fine, see header

    const auto remote = loopback (companionPort);

    if (::connect (s, reinterpret_cast<const sockaddr*> (&remote), sizeof (remote)) != 0)
    {
        close();
        return false;
    }

    // Also makes the socket non-blocking.
    socketEvent = ::WSACreateEvent();

    if (socketEvent == WSA_INVALID_EVENT || ::WSAEventSelect (s, socketEvent, FD_READ) != 0)
    {
        close();
        return false;
    }

    return true;
}

void UdpEndpoint::close() noexcept
{
    if (socketFd != invalidSocket)
        ::closesocket (native (socketFd));
    if (socketEvent != nullptr && socketEvent != WSA_INVALID_EVENT)
        ::WSACloseEvent (socketEvent);
    if (wakeEvent != nullptr)
        ::CloseHandle (wakeEvent);
    if (winsockStarted)
        ::WSACleanup();

    socketFd = invalidSocket;
    socketEvent = nullptr;
    wakeEvent = nullptr;
    winsockStarted = false;
}

bool UdpEndpoint::send (const std::uint8_t* data, std::size_t size) noexcept
{
    if (socketFd == invalidSocket || data == nullptr || size == 0 || size > 65507)
        return false;

    const int sent = ::send (native (socketFd), reinterpret_cast<const char*> (data), static_cast<int> (size), 0);

    if (sent == static_cast<int> (size))
        return true;

    tooLarge = sent == SOCKET_ERROR && ::WSAGetLastError() == WSAEMSGSIZE;
    ++failedSends; // WSAECONNRESET (companion absent), WSAENOBUFS, ... -- dropped, never queued
    return false;
}

long UdpEndpoint::receive (std::uint8_t* buffer, std::size_t capacity) noexcept
{
    if (socketFd == invalidSocket)
        return -1;

    const int length = static_cast<int> (capacity > 0x7fffffff ? 0x7fffffff : capacity);

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const int received = ::recv (native (socketFd), reinterpret_cast<char*> (buffer), length, 0);

        if (received >= 0)
            return static_cast<long> (received);

        const int error = ::WSAGetLastError();

        if (error == WSAEINTR || error == WSAECONNRESET)
            continue; // an ICMP error is consumed by this call; look again

        if (error == WSAEMSGSIZE)
            return static_cast<long> (length); // truncated, as POSIX recv reports it; the decoder rejects it

        return -1;
    }

    return -1;
}

bool UdpEndpoint::wait (int timeoutMs) noexcept
{
    HANDLE handles[2] {};
    DWORD count = 0;
    DWORD socketIndex = MAXDWORD;

    if (socketFd != invalidSocket && socketEvent != nullptr)
    {
        socketIndex = count;
        handles[count++] = socketEvent;
    }

    if (wakeEvent != nullptr)
        handles[count++] = wakeEvent;

    const DWORD timeout = static_cast<DWORD> (timeoutMs > 0 ? timeoutMs : 0);

    if (count == 0)
    {
        ::Sleep (timeout);
        return false;
    }

    const DWORD result = ::WaitForMultipleObjects (count, handles, FALSE, timeout);

    if (result >= WAIT_OBJECT_0 + count)
        return false; // timeout or failure

    // A wake (auto-reset, already consumed) may coincide with a datagram:
    // report the socket as poll() would, without waiting.
    const bool socketSignalled = result - WAIT_OBJECT_0 == socketIndex
                              || (socketIndex != MAXDWORD && ::WaitForSingleObject (socketEvent, 0) == WAIT_OBJECT_0);

    if (! socketSignalled)
        return false;

    // Resets the event. FD_READ is re-armed by every recv(), so a datagram
    // still queued after the bus drains signals the event again.
    WSANETWORKEVENTS events {};
    ::WSAEnumNetworkEvents (native (socketFd), socketEvent, &events);
    return true;
}

void UdpEndpoint::wake() noexcept
{
    if (wakeEvent != nullptr)
        ::SetEvent (wakeEvent);
}

std::uint16_t UdpEndpoint::localPort() const noexcept
{
    if (socketFd == invalidSocket)
        return 0;

    sockaddr_in address {};
    int length = sizeof (address);

    if (::getsockname (native (socketFd), reinterpret_cast<sockaddr*> (&address), &length) != 0)
        return 0;

    return ntohs (address.sin_port);
}

} // namespace mixersend

#else // POSIX

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mixersend
{

namespace
{
    void makeNonBlockingCloExec (int fd) noexcept
    {
        const int flags = ::fcntl (fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl (fd, F_SETFL, flags | O_NONBLOCK);

        // Hosts spawn helper processes; the socket must not leak into them.
        ::fcntl (fd, F_SETFD, FD_CLOEXEC);
    }

    sockaddr_in loopback (std::uint16_t port) noexcept
    {
        sockaddr_in address {};
        address.sin_len = sizeof (address);
        address.sin_family = AF_INET;
        address.sin_port = htons (port);
        address.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        return address;
    }
}

UdpEndpoint::~UdpEndpoint()
{
    close();
}

bool UdpEndpoint::open (std::uint16_t companionPort) noexcept
{
    close();

    int fds[2] = { -1, -1 };

    if (::pipe (fds) != 0)
        return false;

    wakeRead = fds[0];
    wakeWrite = fds[1];
    makeNonBlockingCloExec (wakeRead);
    makeNonBlockingCloExec (wakeWrite);

    socketFd = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socketFd < 0)
        return false;

    makeNonBlockingCloExec (socketFd);

   #ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt (socketFd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
   #endif

    // Enough for a burst of control datagrams; the bus drains every wake.
    const int receiveBuffer = 64 * 1024;
    ::setsockopt (socketFd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer, sizeof (receiveBuffer));

    auto local = loopback (0);
    ::bind (socketFd, reinterpret_cast<const sockaddr*> (&local), sizeof (local)); // failure is fine, see header

    const auto remote = loopback (companionPort);

    if (::connect (socketFd, reinterpret_cast<const sockaddr*> (&remote), sizeof (remote)) != 0)
    {
        ::close (socketFd);
        socketFd = -1;
        return false;
    }

    return true;
}

void UdpEndpoint::close() noexcept
{
    if (socketFd >= 0)
        ::close (socketFd);
    if (wakeRead >= 0)
        ::close (wakeRead);
    if (wakeWrite >= 0)
        ::close (wakeWrite);

    socketFd = wakeRead = wakeWrite = -1;
}

bool UdpEndpoint::send (const std::uint8_t* data, std::size_t size) noexcept
{
    if (socketFd < 0 || data == nullptr || size == 0)
        return false;

    const auto sent = ::send (socketFd, data, size, 0);

    if (sent == static_cast<ssize_t> (size))
        return true;

    tooLarge = sent < 0 && errno == EMSGSIZE;
    ++failedSends; // ECONNREFUSED (companion absent), ENOBUFS, ... -- dropped, never queued
    return false;
}

long UdpEndpoint::receive (std::uint8_t* buffer, std::size_t capacity) noexcept
{
    if (socketFd < 0)
        return -1;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const auto received = ::recv (socketFd, buffer, capacity, 0);

        if (received >= 0)
            return static_cast<long> (received);

        if (errno == EINTR || errno == ECONNREFUSED)
            continue; // an ICMP error is consumed by this call; look again

        return -1;
    }

    return -1;
}

bool UdpEndpoint::wait (int timeoutMs) noexcept
{
    pollfd fds[2] {};
    int count = 0;

    if (socketFd >= 0)
        fds[count++] = { socketFd, POLLIN, 0 };

    if (wakeRead >= 0)
        fds[count++] = { wakeRead, POLLIN, 0 };

    if (count == 0)
    {
        ::usleep (static_cast<useconds_t> (timeoutMs > 0 ? timeoutMs : 0) * 1000u);
        return false;
    }

    const int ready = ::poll (fds, static_cast<nfds_t> (count), timeoutMs < 0 ? 0 : timeoutMs);

    if (ready <= 0)
        return false;

    bool readable = false;

    for (int i = 0; i < count; ++i)
    {
        if (fds[i].fd == wakeRead && (fds[i].revents & POLLIN) != 0)
        {
            std::uint8_t drain[64];
            while (::read (wakeRead, drain, sizeof (drain)) > 0) {}
        }
        else if (fds[i].fd == socketFd && (fds[i].revents & (POLLIN | POLLERR)) != 0)
        {
            readable = true;
        }
    }

    return readable;
}

void UdpEndpoint::wake() noexcept
{
    if (wakeWrite >= 0)
    {
        const std::uint8_t byte = 1;
        [[maybe_unused]] const auto written = ::write (wakeWrite, &byte, 1); // full pipe = already awake
    }
}

std::uint16_t UdpEndpoint::localPort() const noexcept
{
    if (socketFd < 0)
        return 0;

    sockaddr_in address {};
    socklen_t length = sizeof (address);

    if (::getsockname (socketFd, reinterpret_cast<sockaddr*> (&address), &length) != 0)
        return 0;

    return ntohs (address.sin_port);
}

} // namespace mixersend

#endif // _WIN32
