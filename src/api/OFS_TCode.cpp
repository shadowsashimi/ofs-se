#include "OFS_TCode.h"

#include "OFS_Util.h"
#include "stb_sprintf.h"

#include <cmath>
#include <cstring>
#include <vector>

#if defined(WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace OFS_TCode
{

int32_t FormatAxis(char* buffer, size_t size, const char* channel, float position, int32_t intervalMs) noexcept
{
    const int32_t value = Util::Clamp((int32_t)std::lround(position * (float)OutputMaximum), 0, OutputMaximum);
    if (intervalMs > 0) {
        return stbsp_snprintf(buffer, (int)size, "%s%03dI%d", channel, value, intervalMs);
    }
    return stbsp_snprintf(buffer, (int)size, "%s%03d", channel, value);
}

bool IsDirty(float value, float lastValue) noexcept
{
    if (!std::isfinite(lastValue)) return std::isfinite(value);
    return std::abs(value - lastValue) * (float)(OutputMaximum + 1) >= 1.f;
}

// ------------------------------------------------------------------ sockets --

#if defined(WIN32)
using SocketHandleType = SOCKET;
#else
using SocketHandleType = int;
#endif

#if defined(WIN32)
// Winsock has to be started before any socket call and stopped at the end.
// civetweb does its own, and the counts are per process, so this is a second
// tenant rather than an owner.
static bool startWinsock() noexcept
{
    static bool started = false;
    if (started) return true;
    WSADATA data;
    started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    return started;
}
#endif

Link::~Link() noexcept
{
    Close();
}

void Link::Close() noexcept
{
    if (handle == -1) return;
#if defined(WIN32)
    if (transport == Transport::Serial) {
        CloseHandle((HANDLE)handle);
    }
    else {
        closesocket((SOCKET)handle);
    }
#else
    close((int)handle);
#endif
    handle = -1;
    addressLength = 0;
}

bool Link::Open(Transport wanted, const std::string& port, int32_t baudOrPort) noexcept
{
    Close();
    transport = wanted;
    error.clear();

    if (wanted == Transport::Serial) {
#if defined(WIN32)
        // The \\.\ prefix is what reaches COM10 and above; without it the name
        // is read as a DOS device and anything past COM9 cannot be opened.
        const std::string path = port.rfind("\\\\.\\", 0) == 0 ? port : ("\\\\.\\" + port);
        HANDLE com = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr);
        if (com == INVALID_HANDLE_VALUE) {
            error = "Could not open " + port + ". Is it the right port, and is anything else using it?";
            return false;
        }

        DCB state = {};
        state.DCBlength = sizeof(state);
        if (!GetCommState(com, &state)) {
            CloseHandle(com);
            error = "Could not read the state of " + port + ".";
            return false;
        }
        state.BaudRate = (DWORD)baudOrPort;
        state.ByteSize = 8;
        state.Parity = NOPARITY;
        state.StopBits = ONESTOPBIT;
        // No flow control: a stroker listens, it does not ask to be waited for.
        state.fOutxCtsFlow = FALSE;
        state.fOutxDsrFlow = FALSE;
        state.fDtrControl = DTR_CONTROL_ENABLE;
        state.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(com, &state)) {
            CloseHandle(com);
            error = "Could not set " + port + " to " + std::to_string(baudOrPort) + " baud.";
            return false;
        }

        // Writes must not block the frame they are sent from: a device that
        // stops reading would otherwise stop the app.
        COMMTIMEOUTS timeouts = {};
        timeouts.ReadIntervalTimeout = MAXDWORD;
        timeouts.WriteTotalTimeoutConstant = 50;
        SetCommTimeouts(com, &timeouts);

        handle = (intptr_t)com;
        return true;
#else
        int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            error = "Could not open " + port + ".";
            return false;
        }
        termios tty = {};
        if (tcgetattr(fd, &tty) != 0) {
            close(fd);
            error = "Could not read the state of " + port + ".";
            return false;
        }
        cfmakeraw(&tty);
        speed_t speed = B115200;
        switch (baudOrPort) {
            case 9600: speed = B9600; break;
            case 19200: speed = B19200; break;
            case 38400: speed = B38400; break;
            case 57600: speed = B57600; break;
            case 115200: speed = B115200; break;
            default: speed = B115200; break;
        }
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
        tty.c_cflag |= CLOCAL | CREAD;
        tty.c_cflag &= ~CRTSCTS;
        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
            close(fd);
            error = "Could not set " + port + " to " + std::to_string(baudOrPort) + " baud.";
            return false;
        }
        handle = (intptr_t)fd;
        return true;
#endif
    }

    // Tcp and Udp from here.
#if defined(WIN32)
    if (!startWinsock()) {
        error = "Windows sockets could not be started.";
        return false;
    }
#endif

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = wanted == Transport::Tcp ? SOCK_STREAM : SOCK_DGRAM;
    hints.ai_protocol = wanted == Transport::Tcp ? IPPROTO_TCP : IPPROTO_UDP;

    addrinfo* found = nullptr;
    const std::string service = std::to_string(baudOrPort);
    if (getaddrinfo(port.c_str(), service.c_str(), &hints, &found) != 0 || found == nullptr) {
        error = "Could not find " + port + ".";
        return false;
    }

    bool opened = false;
    for (addrinfo* at = found; at != nullptr; at = at->ai_next) {
#if defined(WIN32)
        SOCKET sock = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (sock == INVALID_SOCKET) continue;
#else
        int sock = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (sock < 0) continue;
#endif
        if (wanted == Transport::Tcp) {
            if (connect(sock, at->ai_addr, (int)at->ai_addrlen) != 0) {
#if defined(WIN32)
                closesocket(sock);
#else
                close(sock);
#endif
                continue;
            }
            // Every command is one small line, and waiting to bundle them
            // would add delay to exactly the thing that must not have any.
            int noDelay = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        }
        else {
            // Nothing to connect: the address is kept and used per datagram.
            if (at->ai_addrlen <= sizeof(address)) {
                memcpy(address, at->ai_addr, at->ai_addrlen);
                addressLength = (int32_t)at->ai_addrlen;
            }
        }
        handle = (intptr_t)sock;
        opened = true;
        break;
    }
    freeaddrinfo(found);

    if (!opened) {
        error = wanted == Transport::Tcp
            ? ("Nothing answered at " + port + ":" + service + ".")
            : ("Could not open a socket for " + port + ":" + service + ".");
    }
    return opened;
}

bool Link::Write(const char* data, size_t length) noexcept
{
    if (handle == -1 || length == 0) return false;

    if (transport == Transport::Serial) {
#if defined(WIN32)
        DWORD written = 0;
        if (!WriteFile((HANDLE)handle, data, (DWORD)length, &written, nullptr)) {
            error = "The serial port stopped accepting commands.";
            Close();
            return false;
        }
        return written == (DWORD)length;
#else
        const ssize_t written = write((int)handle, data, length);
        if (written < 0) {
            error = "The serial port stopped accepting commands.";
            Close();
            return false;
        }
        return (size_t)written == length;
#endif
    }

    int sent = 0;
    if (transport == Transport::Udp) {
        sent = (int)sendto((SocketHandleType)handle, data, (int)length, 0,
            (const sockaddr*)address, addressLength);
    }
    else {
        sent = (int)send((SocketHandleType)handle, data, (int)length, 0);
    }
    if (sent < 0) {
        error = "The connection was lost.";
        Close();
        return false;
    }
    return (size_t)sent == length;
}

void ListSerialPorts(std::vector<std::string>& outPorts) noexcept
{
    outPorts.clear();
#if defined(WIN32)
    // Asking the registry rather than probing: opening a port to see whether
    // it exists takes it from whatever else has it open.
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    char name[256];
    char value[256];
    DWORD index = 0;
    for (;;) {
        DWORD nameSize = sizeof(name);
        DWORD valueSize = sizeof(value);
        DWORD type = 0;
        const LONG result = RegEnumValueA(key, index, name, &nameSize, nullptr, &type,
            (LPBYTE)value, &valueSize);
        if (result != ERROR_SUCCESS) break;
        if (type == REG_SZ) outPorts.emplace_back(value);
        index += 1;
    }
    RegCloseKey(key);
#endif
}

}
