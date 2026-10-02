#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Talking TCode to a stroker that speaks it directly - an OSR2, an SR6, and
// the other builds of the same family - rather than through Intiface.
//
// The protocol is a line of commands, one per axis: "L0500I100" puts axis L0
// at 500 of 999 over the next 100 milliseconds. Several axes go in one line,
// separated by spaces and ended by a newline, which is how a six axis move
// arrives as one instruction rather than six that drift apart.
//
// Ported from MultiFunPlayer (MIT licensed), which is where the format, the
// three digit values, the interval, and sending only what changed come from.
// Its licence is in THIRD_PARTY_NOTICES.md.
namespace OFS_TCode
{

// Values are three digits, so 999 is the top of an axis's travel.
static constexpr int32_t OutputMaximum = 999;

// Writes one axis command, "L0500I100", into the buffer. Position is 0..1 of
// the axis's travel and intervalMs is how long the device should take.
// Returns the length written.
int32_t FormatAxis(char* buffer, size_t size, const char* channel, float position, int32_t intervalMs) noexcept;

// Whether a new value differs enough to be worth sending: less than one step
// of the three digits is a command that asks for what the device is doing.
bool IsDirty(float value, float lastValue) noexcept;

// Where the commands go. Each is a plain byte sink; only opening and closing
// them differs.
enum class Transport : int32_t
{
    // A COM port. A stroker on USB is one of these, and so is one paired over
    // Bluetooth, which Windows presents as a serial port of its own.
    Serial,
    Tcp,
    Udp,
};

// One open connection. Not thread safe: the sender owns it.
class Link
{
private:
    Transport transport = Transport::Serial;
    // A HANDLE on Windows, a file descriptor elsewhere. Kept as an intptr so
    // the header carries no platform headers with it.
    intptr_t handle = -1;
    std::string error;
    // Where a datagram goes, kept as bytes so the header stays free of
    // sockaddr. Only used by Udp.
    unsigned char address[128] = {};
    int32_t addressLength = 0;

public:
    ~Link() noexcept;

    // port is a COM port name for Serial ("COM3"), or a host for Tcp and Udp.
    bool Open(Transport transport, const std::string& port, int32_t baudOrPort) noexcept;
    void Close() noexcept;
    bool IsOpen() const noexcept { return handle != -1; }
    bool Write(const char* data, size_t length) noexcept;
    const std::string& Error() const noexcept { return error; }
};

// The COM ports the machine has, for the panel to offer. Empty elsewhere than
// Windows, where the panel asks for a device path instead.
void ListSerialPorts(std::vector<std::string>& outPorts) noexcept;

}
