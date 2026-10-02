#pragma once

#include "OFS_TCode.h"

#include <cstdint>
#include <limits>
#include <set>
#include <mutex>
#include <string>
#include <vector>

// Drives a real device from the script while it plays, two ways:
//
//   Intiface Central, over a websocket, speaking Buttplug. Intiface owns the
//   bluetooth connection, so every device it supports comes along for free,
//   and a move is a target with a duration to reach it in.
//
//   A TCode machine - an OSR2, an SR6 - spoken to directly, down a serial port
//   or over the network, a hundred lines a second with a value per axis. All
//   six axes are driven, from the project's own axis scripts.
//
// The TCode side follows MultiFunPlayer (MIT licensed): the command format,
// the send rate, sending only what changed, and the safety - per axis travel
// limits, a speed limit, and easing an axis home when nothing drives it.
//
// The websocket connection runs on civetweb's own thread, so everything it
// touches is behind a mutex and the UI only ever reads a copy.
class OFS_DeviceLink
{
public:
    struct Device
    {
        int32_t index = 0;
        std::string name;
        // Only devices that can be told a position are any use here. A device
        // that can only vibrate is listed, and left alone.
        bool canMove = false;
        bool enabled = true;
    };

    enum class Status : int32_t
    {
        Off,
        Connecting,
        Connected,
        Failed,
    };

    // What one TCode axis is doing between sends.
    struct AxisRuntime
    {
        // Where it is now, and what was last put on the wire, which is what
        // decides whether a new value is worth sending at all.
        float value = std::numeric_limits<float>::quiet_NaN();
        float sentValue = std::numeric_limits<float>::quiet_NaN();
        // How long nothing has driven it, and where it started easing home.
        float idleTime = 0.f;
        float homeFrom = std::numeric_limits<float>::quiet_NaN();
    };

private:
    struct mg_connection* connection = nullptr;
    // Guards everything below, which the websocket thread writes and the UI
    // thread reads.
    mutable std::mutex mutex;
    Status status = Status::Off;
    std::string message;
    std::vector<Device> devices;
    uint32_t nextMessageId = 1;
    // Time the last move was sent for, so the same move is not sent twice, and
    // when it was sent, for the trickle while paused.
    float lastSentAt = -1.f;
    float lastSentPosition = -1.f;
    uint64_t lastSendTicks = 0;
    int32_t movesSent = 0;
    bool sawServerInfo = false;

    uint32_t stateHandle = 0xFFFF'FFFF;

    OFS_TCode::Link link;
    std::vector<AxisRuntime> axisState;
    float sinceSend = 0.f;
    // What the send rate actually comes to. Lines go out from the frame loop,
    // so the interval asked for is a floor: at 150 frames a second the closest
    // it can get to 10ms is 13. Measured rather than assumed, because it is
    // what the machine is really being fed.
    float rateWindow = 0.f;
    int32_t rateCount = 0;
    float sendsPerSecond = 0.f;
    // Which channels have moved since connecting, meaning a script drove them
    // rather than their being set once with everything else. For the UI test
    // driver, which is how driving more than one axis is checked.
    std::set<std::string> channelsSent;
    std::vector<std::string> serialPorts;

    void send(const std::string& json) noexcept;
    void ensureAxes() noexcept;
    void sendTCode(float deltaTime) noexcept;
    void sendTCodeHome() noexcept;
    void drawIntifaceSettings() noexcept;
    void drawTCodeSettings() noexcept;
    void drawAxisTable() noexcept;
    void handleMessage(const char* data, size_t length) noexcept;
    void sendMove(int32_t deviceIndex, float position, float durationMs) noexcept;
    float positionFor(float scriptPos) const noexcept;

public:
    OFS_DeviceLink() noexcept;
    ~OFS_DeviceLink() noexcept;

    void Connect() noexcept;
    void Disconnect() noexcept;
    void StartScanning() noexcept;
    // Sends a stop to every device, for the button and for pausing.
    void StopDevices() noexcept;

    // Called every frame: works out where the script is going next and tells
    // the devices to move there.
    void Update(float deltaTime) noexcept;
    void DrawWindow(bool* open) noexcept;

    static constexpr const char* WindowId = "###DEVICES";

    inline Status CurrentStatus() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return status;
    }
    inline int32_t DeviceCount() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return (int32_t)devices.size();
    }
    // The channels sent since connecting, in order, as "L0,R1".
    std::string ChannelsSent() const noexcept;
    // Every axis and where it last went, as "L0:813,R0:501", for the UI test
    // driver to check what a machine is actually being told.
    std::string AxisReport() const noexcept;
    // Whether every enabled axis has reached its rest position, which is what
    // easing home ends at.
    bool AllAxesHome() const noexcept;
    // Where the stroke axis last went, 0 to 999, or -1 when it has not been
    // sent yet. The one axis every project has.
    int32_t StrokeValue() const noexcept;
    inline int32_t MovesSent() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        return movesSent;
    }

    // civetweb hands messages back through these.
    int OnData(int bits, char* data, size_t length) noexcept;
    void OnClose() noexcept;
};
