#pragma once

#include "OFS_StateHandle.h"

#include <string>
#include <vector>

// Which way the device is reached.
// ATTENTION: no reordering, saved as a number
enum DeviceBackend : int32_t
{
    // Buttplug over a websocket, which is Intiface Central's business.
    DeviceBackendIntiface,
    // TCode down a COM port: a stroker on USB, or one paired over Bluetooth,
    // which Windows presents as a serial port of its own.
    DeviceBackendSerial,
    // TCode over the network, to a stroker on wifi.
    DeviceBackendNetwork,
    DeviceBackendCount
};

// One TCode axis: which script drives it, where it is allowed to go, and how
// fast it may get there. The limits are per axis because a machine's axes are
// not alike - a full stroke is fine where a full roll would hit a stop.
struct DeviceAxisSettings
{
    // The TCode channel, "L0" and the rest.
    std::string channel;
    // The script that drives it, by the name OFS gives an axis script.
    std::string script;
    bool enabled = true;
    // Send the script upside down, for a machine mounted the other way round.
    bool invert = false;
    // The part of the axis's travel the script is mapped into, in percent.
    int32_t rangeMin = 0;
    int32_t rangeMax = 100;
    // Units of travel per second, 0 for no limit. A script can ask for a
    // stroke faster than the machine can make, and what comes of that is a
    // bang rather than a stroke.
    int32_t speedLimit = 0;
    // Ease back to restPosition when nothing is driving this axis.
    bool autoHome = true;
    int32_t restPosition = 50;
};

struct DeviceLinkState
{
    static constexpr auto StateName = "DeviceLinkState";

    int32_t backend = DeviceBackendIntiface;

    // Intiface.
    std::string address = "127.0.0.1";
    int32_t port = 12345;

    // Serial, and Bluetooth by way of a serial port.
    std::string serialPort = "COM3";
    int32_t baudRate = 115200;

    // Network. TCode goes over either, and which one depends on the firmware.
    std::string tcodeHost = "192.168.1.100";
    int32_t tcodePort = 8000;
    bool useUdp = true;

    // How often a TCode line goes out. Ten milliseconds is a hundred a second,
    // which is what MultiFunPlayer settled on and what the firmware expects to
    // be fed.
    int32_t sendIntervalMs = 10;
    // How long an axis waits with nothing driving it before it eases home, and
    // how long it takes to get there.
    float autoHomeDelaySeconds = 2.f;
    float autoHomeDurationSeconds = 0.6f;

    // How far ahead of the stroke a device is told, for Intiface, which is
    // given a target and a duration rather than a position every few
    // milliseconds.
    int32_t latencyMs = 100;
    int32_t rangeMin = 0;
    int32_t rangeMax = 100;
    bool followWhilePaused = true;

    std::vector<DeviceAxisSettings> axes;

    static inline DeviceLinkState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<DeviceLinkState>(stateHandle).Get();
    }
};

REFL_TYPE(DeviceAxisSettings)
    REFL_FIELD(channel)
    REFL_FIELD(script)
    REFL_FIELD(enabled)
    REFL_FIELD(invert)
    REFL_FIELD(rangeMin)
    REFL_FIELD(rangeMax)
    REFL_FIELD(speedLimit)
    REFL_FIELD(autoHome)
    REFL_FIELD(restPosition)
REFL_END

REFL_TYPE(DeviceLinkState)
    REFL_FIELD(backend)
    REFL_FIELD(address)
    REFL_FIELD(port)
    REFL_FIELD(serialPort)
    REFL_FIELD(baudRate)
    REFL_FIELD(tcodeHost)
    REFL_FIELD(tcodePort)
    REFL_FIELD(useUdp)
    REFL_FIELD(sendIntervalMs)
    REFL_FIELD(autoHomeDelaySeconds)
    REFL_FIELD(autoHomeDurationSeconds)
    REFL_FIELD(latencyMs)
    REFL_FIELD(rangeMin)
    REFL_FIELD(rangeMax)
    REFL_FIELD(followWhilePaused)
    REFL_FIELD(axes)
REFL_END
