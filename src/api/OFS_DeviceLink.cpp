#include "OFS_DeviceLink.h"

#include "OpenFunscripter.h"
#include "OFS_ImGui.h"
#include "OFS_Profiling.h"
#include "state/DeviceLinkState.h"

#include "civetweb.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "nlohmann/json.hpp"

#include "SDL_timer.h"
#include "stb_sprintf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// Buttplug names its protocol version in every handshake; 3 is what Intiface
// Central speaks.
static constexpr int32_t ProtocolVersion = 3;

static int onDataThunk(struct mg_connection* conn, int bits, char* data, size_t length, void* userData) noexcept
{
    (void)conn;
    return ((OFS_DeviceLink*)userData)->OnData(bits, data, length);
}

static void onCloseThunk(const struct mg_connection* conn, void* userData) noexcept
{
    (void)conn;
    ((OFS_DeviceLink*)userData)->OnClose();
}

OFS_DeviceLink::OFS_DeviceLink() noexcept
{
    stateHandle = OFS_AppState<DeviceLinkState>::Register(DeviceLinkState::StateName);
}

OFS_DeviceLink::~OFS_DeviceLink() noexcept
{
    Disconnect();
}

void OFS_DeviceLink::send(const std::string& json) noexcept
{
    if (connection == nullptr) return;
    mg_websocket_client_write(connection, MG_WEBSOCKET_OPCODE_TEXT, json.c_str(), json.size());
}

void OFS_DeviceLink::Connect() noexcept
{
    Disconnect();
    auto& state = DeviceLinkState::State(stateHandle);

    if (state.backend != DeviceBackendIntiface) {
        ensureAxes();
        const bool serial = state.backend == DeviceBackendSerial;
        const auto transport = serial
            ? OFS_TCode::Transport::Serial
            : (state.useUdp ? OFS_TCode::Transport::Udp : OFS_TCode::Transport::Tcp);
        const std::string where = serial ? state.serialPort : state.tcodeHost;
        const int32_t number = serial ? state.baudRate : state.tcodePort;

        std::lock_guard<std::mutex> lock(mutex);
        if (link.Open(transport, where, number)) {
            status = Status::Connected;
            message = serial
                ? ("Sending TCode to " + where + " at " + std::to_string(number) + " baud.")
                : ("Sending TCode to " + where + ":" + std::to_string(number)
                    + (state.useUdp ? " over UDP." : " over TCP."));
            movesSent = 0;
            sinceSend = 0.f;
            channelsSent.clear();
            axisState.assign(state.axes.size(), AxisRuntime());
        }
        else {
            status = Status::Failed;
            message = link.Error();
        }
        return;
    }

    char error[256] = { 0 };
    {
        std::lock_guard<std::mutex> lock(mutex);
        status = Status::Connecting;
        message = "Connecting to " + state.address + ":" + std::to_string(state.port) + "...";
        devices.clear();
        movesSent = 0;
        sawServerInfo = false;
    }

    connection = mg_connect_websocket_client(state.address.c_str(), state.port, 0,
        error, sizeof(error), "/", nullptr, onDataThunk, onCloseThunk, this);

    if (connection == nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        status = Status::Failed;
        message = error[0] != 0
            ? std::string(error)
            : std::string("Could not reach Intiface. Is it running, with its server started?");
        return;
    }

    // Buttplug opens with the client naming itself and the version it speaks.
    nlohmann::json handshake = nlohmann::json::array();
    handshake.push_back({ { "RequestServerInfo", {
        { "Id", (int32_t)nextMessageId++ },
        { "ClientName", "OFS-SE" },
        { "MessageVersion", ProtocolVersion } } } });
    send(handshake.dump());
}

void OFS_DeviceLink::Disconnect() noexcept
{
    if (link.IsOpen()) {
        // Home before closing: a machine holds whatever position it was last
        // given, and what it was last given is wherever the script stopped.
        sendTCodeHome();
        link.Close();
    }
    if (connection != nullptr) {
        StopDevices();
        mg_close_connection(connection);
        connection = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex);
    status = Status::Off;
    message.clear();
    devices.clear();
    lastSentAt = -1.f;
    lastSentPosition = -1.f;
}

void OFS_DeviceLink::StartScanning() noexcept
{
    if (connection == nullptr) return;
    nlohmann::json msg = nlohmann::json::array();
    msg.push_back({ { "StartScanning", { { "Id", (int32_t)nextMessageId++ } } } });
    send(msg.dump());
}

void OFS_DeviceLink::StopDevices() noexcept
{
    if (link.IsOpen()) {
        sendTCodeHome();
        return;
    }
    if (connection == nullptr) return;
    nlohmann::json msg = nlohmann::json::array();
    msg.push_back({ { "StopAllDevices", { { "Id", (int32_t)nextMessageId++ } } } });
    send(msg.dump());
    std::lock_guard<std::mutex> lock(mutex);
    lastSentAt = -1.f;
    lastSentPosition = -1.f;
}

int OFS_DeviceLink::OnData(int bits, char* data, size_t length) noexcept
{
    const int opcode = bits & 0xF;
    if (opcode == MG_WEBSOCKET_OPCODE_TEXT && data != nullptr && length > 0) {
        handleMessage(data, length);
    }
    // Anything but zero keeps the connection open.
    return 1;
}

void OFS_DeviceLink::OnClose() noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    connection = nullptr;
    if (status != Status::Off) {
        status = Status::Failed;
        message = "Intiface closed the connection.";
    }
    devices.clear();
}

void OFS_DeviceLink::handleMessage(const char* data, size_t length) noexcept
{
    auto parsed = nlohmann::json::parse(data, data + length, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) return;

    // Buttplug sends an array of messages, each an object of one named kind.
    for (const auto& entry : parsed) {
        if (!entry.is_object()) continue;
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            const std::string& kind = it.key();
            const auto& body = it.value();

            if (kind == "ServerInfo") {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    status = Status::Connected;
                    sawServerInfo = true;
                    const auto name = body.value("ServerName", std::string("Intiface"));
                    message = "Connected to " + name + ".";
                }
                nlohmann::json ask = nlohmann::json::array();
                ask.push_back({ { "RequestDeviceList", { { "Id", (int32_t)nextMessageId++ } } } });
                send(ask.dump());
            }
            else if (kind == "DeviceList" || kind == "DeviceAdded") {
                auto add = [this](const nlohmann::json& device) noexcept {
                    Device found;
                    found.index = device.value("DeviceIndex", 0);
                    found.name = device.value("DeviceName", std::string("Device"));
                    if (device.contains("DeviceMessages")) {
                        found.canMove = device["DeviceMessages"].contains("LinearCmd");
                    }
                    std::lock_guard<std::mutex> lock(mutex);
                    for (auto& existing : devices) {
                        if (existing.index == found.index) {
                            const bool wasEnabled = existing.enabled;
                            existing = found;
                            existing.enabled = wasEnabled;
                            return;
                        }
                    }
                    devices.push_back(found);
                };

                if (kind == "DeviceAdded") add(body);
                else if (body.contains("Devices") && body["Devices"].is_array()) {
                    for (const auto& device : body["Devices"]) add(device);
                }
            }
            else if (kind == "DeviceRemoved") {
                const int32_t index = body.value("DeviceIndex", -1);
                std::lock_guard<std::mutex> lock(mutex);
                devices.erase(std::remove_if(devices.begin(), devices.end(),
                    [index](const Device& device) noexcept { return device.index == index; }),
                    devices.end());
            }
            else if (kind == "Error") {
                std::lock_guard<std::mutex> lock(mutex);
                message = body.value("ErrorMessage", std::string("Intiface reported an error."));
            }
        }
    }
}

float OFS_DeviceLink::positionFor(float scriptPos) const noexcept
{
    const auto& state = DeviceLinkState::State(stateHandle);
    const float low = (float)std::min(state.rangeMin, state.rangeMax) / 100.f;
    const float high = (float)std::max(state.rangeMin, state.rangeMax) / 100.f;
    const float within = Util::Clamp(scriptPos / 100.f, 0.f, 1.f);
    return low + ((high - low) * within);
}

void OFS_DeviceLink::sendMove(int32_t deviceIndex, float position, float durationMs) noexcept
{
    nlohmann::json msg = nlohmann::json::array();
    nlohmann::json vector = {
        { "Index", 0 },
        { "Duration", (int32_t)std::max(1.f, durationMs) },
        { "Position", Util::Clamp(position, 0.f, 1.f) }
    };
    msg.push_back({ { "LinearCmd", {
        { "Id", (int32_t)nextMessageId++ },
        { "DeviceIndex", deviceIndex },
        { "Vectors", nlohmann::json::array({ vector }) } } } });
    send(msg.dump());
}

void OFS_DeviceLink::Update(float deltaTime) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto* appPtr = OpenFunscripter::ptr;
    if (link.IsOpen()) {
        if (appPtr->LoadedProject->IsValid()) sendTCode(deltaTime);
        return;
    }
    if (connection == nullptr) return;
    auto app = OpenFunscripter::ptr;
    if (!app->LoadedProject->IsValid()) return;

    auto& state = DeviceLinkState::State(stateHandle);
    std::vector<int32_t> targets;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (status != Status::Connected) return;
        for (const auto& device : devices) {
            if (device.enabled && device.canMove) targets.push_back(device.index);
        }
    }
    if (targets.empty()) return;

    auto script = app->ActiveFunscript();
    const float now = (float)app->player->CurrentTime();
    // The device needs telling before the stroke is due, by however long the
    // link and the hardware take to act on it.
    const float lead = (float)state.latencyMs / 1000.f;

    if (app->player->IsPaused()) {
        if (!state.followWhilePaused) return;
        // Scrubbing: the device follows the playhead itself rather than the
        // stroke coming up, at a rate a device can keep up with.
        const uint64_t ticks = SDL_GetTicks64();
        if (ticks - lastSendTicks < 100) return;
        const auto* action = script->GetClosestAction(now);
        if (action == nullptr) return;
        const float position = positionFor((float)action->pos);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (std::abs(position - lastSentPosition) < 0.01f) return;
            lastSentPosition = position;
            lastSendTicks = ticks;
            movesSent += 1;
        }
        for (int32_t index : targets) sendMove(index, position, 150.f);
        return;
    }

    const auto* next = script->GetNextActionAhead(now + lead);
    if (next == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        // One command per stroke: the same target sent every frame would be a
        // few hundred messages a second and a device that never arrives.
        if (next->atS == lastSentAt) return;
        lastSentAt = next->atS;
        lastSentPosition = positionFor((float)next->pos);
        lastSendTicks = SDL_GetTicks64();
        movesSent += 1;
    }
    const float durationMs = std::max(1.f, (next->atS - (now + lead)) * 1000.f);
    for (int32_t index : targets) sendMove(index, positionFor((float)next->pos), durationMs);
}

void OFS_DeviceLink::drawIntifaceSettings() noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Through Intiface Central, which holds the bluetooth connection. Start its "
                        "server, then connect here.");
    ImGui::PopTextWrapPos();

    ImGui::TextDisabled("Address");
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##DeviceAddress", &state.address);
    OFS::StepperInt("Port", "##DevicePort", &state.port, 1, 1, 65535);
}

void OFS_DeviceLink::drawTCodeSettings() noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);

    if (state.backend == DeviceBackendSerial) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Straight down a serial port. A machine on USB is one of these, and so "
                            "is one paired over Bluetooth, which Windows gives a port of its own.");
        ImGui::PopTextWrapPos();

        ImGui::TextDisabled("Port");
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##SerialPort", state.serialPort.c_str())) {
            // Read when the list is opened rather than every frame: it asks
            // Windows what serial ports exist.
            if (ImGui::IsWindowAppearing()) OFS_TCode::ListSerialPorts(serialPorts);
            for (const auto& port : serialPorts) {
                if (ImGui::Selectable(port.c_str(), port == state.serialPort)) {
                    state.serialPort = port;
                }
            }
            if (serialPorts.empty()) ImGui::TextDisabled("No serial ports found.");
            ImGui::EndCombo();
        }
        OFS::Tooltip("A Bluetooth machine appears here once it is paired in Windows.");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("##SerialPortTyped", &state.serialPort);
        OFS::Tooltip("Or type it: COM3, or a device path on Linux.");
        OFS::StepperInt("Baud", "##SerialBaud", &state.baudRate, 9600, 9600, 1000000);
        OFS::Tooltip("115200 is what the usual firmware listens at.");
    }
    else {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Over the network, to a machine on wifi. Which of UDP and TCP, and "
                            "which port, is whatever its firmware was set up for.");
        ImGui::PopTextWrapPos();

        ImGui::TextDisabled("Address");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("##TCodeHost", &state.tcodeHost);
        OFS::StepperInt("Port", "##TCodePort", &state.tcodePort, 1, 1, 65535);
        {
            static constexpr const char* labels[2] = { "UDP", "TCP" };
            static constexpr const char* tips[2] = {
                "Fire and forget, which is what most wifi firmware listens for.",
                "A connection, for firmware that wants one.",
            };
            const int32_t picked = OFS::SegmentedControl("##TCodeProtocol", labels, tips, 2,
                state.useUdp ? 0 : 1);
            if (picked >= 0) state.useUdp = (picked == 0);
        }
    }

    ImGui::Spacing();
    OFS::StepperInt("Send every (ms)", "##SendInterval", &state.sendIntervalMs, 1, 3, 100);
    OFS::Tooltip("How often a line of TCode goes out. 10ms is a hundred a second, which is what "
                 "the firmware expects to be fed.");
}

// One row per axis: what drives it, how far it may go, and how fast. The
// limits are the difference between a script a machine can follow and one it
// answers with a bang, so they are in front of you rather than in a submenu.
void OFS_DeviceLink::drawAxisTable() noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);
    ensureAxes();

    if (!ImGui::BeginTable("##Axes", 6,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, em * 1.6f);
    ImGui::TableSetupColumn("Axis", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("Lowest", ImGuiTableColumnFlags_WidthFixed, em * 3.6f);
    ImGui::TableSetupColumn("Highest", ImGuiTableColumnFlags_WidthFixed, em * 3.6f);
    ImGui::TableSetupColumn("Rest", ImGuiTableColumnFlags_WidthFixed, em * 3.6f);
    ImGui::TableSetupColumn("Speed cap", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < state.axes.size(); i += 1) {
        auto& axis = state.axes[i];
        ImGui::PushID((int32_t)i);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Checkbox("##on", &axis.enabled);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(axis.script.c_str());
        OFS::TooltipFmt("TCode %s, driven by the %s script. %s", axis.channel.c_str(),
            axis.script.c_str(),
            i == 0 ? "The project's first script is the stroke."
                   : "Add one from Project > Add > Shortcuts.");

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::DragInt("##min", &axis.rangeMin, 1.f, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::DragInt("##max", &axis.rangeMax, 1.f, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.f);
        ImGui::DragInt("##rest", &axis.restPosition, 1.f, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
        OFS::Tooltip("Where it eases back to when nothing is driving it.");

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(em * 6.f);
        ImGui::DragInt("##speed", &axis.speedLimit, 5.f, 0, 2000,
            axis.speedLimit > 0 ? "%d/s" : "off", ImGuiSliderFlags_AlwaysClamp);
        OFS::Tooltip("The fastest this axis may move, in percent of its travel a second. "
                     "Off lets the script ask for whatever it likes.");
        ImGui::SameLine();
        ImGui::Checkbox("flip", &axis.invert);
        OFS::Tooltip("Sends this axis upside down.");

        ImGui::PopID();
    }
    ImGui::EndTable();
}

// Shown the first time Connect is pressed. Device playback is new, and a
// machine doing something unexpected is felt rather than seen, so it asks
// once, before anything moves, and is not asked again once accepted.
static constexpr const char* SafetyNoteId = "Before you connect###deviceSafetyNote";

void OFS_DeviceLink::drawSafetyNote() noexcept
{
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    safetyNoteOpen = ImGui::BeginPopupModal(SafetyNoteId, nullptr,
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
    if (!safetyNoteOpen) return;

    const float em = ImGui::GetFontSize();
    ImGui::PushTextWrapPos(em * 30.f);
    ImGui::TextUnformatted("Device playback is new, and has not been tested on every device, "
                           "firmware or connection type. The motion might not always be what "
                           "you expect.");
    ImGui::Spacing();
    ImGui::TextUnformatted("Before using it on yourself:");
    ImGui::BulletText("Try it first with nobody attached.");
    ImGui::BulletText("Start with a low speed limit and narrow travel limits.");
    ImGui::BulletText("Keep the power switch, or Stop the devices, within reach.");
    ImGui::Spacing();
    ImGui::TextDisabled("You use it at your own risk. This is only shown once.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (ImGui::Button("I understand, connect", ImVec2(em * 12.f, 0.f))) {
        DeviceLinkState::State(stateHandle).safetyNoteAccepted = true;
        ImGui::CloseCurrentPopup();
        Connect();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 8.f, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void OFS_DeviceLink::DrawWindow(bool* open) noexcept
{
    if (open != nullptr && !*open) {
        safetyNoteOpen = false;
        return;
    }
    OFS_PROFILE(__FUNCTION__);
    auto& state = DeviceLinkState::State(stateHandle);

    {
        const auto* viewport = ImGui::GetMainViewport();
        const float em = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(em * 42.f, em * 30.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    }
    ImGui::Begin("Devices###DEVICES", open, ImGuiWindowFlags_None);

    Status currentStatus;
    std::string currentMessage;
    std::vector<Device> currentDevices;
    int32_t currentMoves = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        currentStatus = status;
        currentMessage = message;
        currentDevices = devices;
        currentMoves = movesSent;
    }
    const bool connected = currentStatus == Status::Connected;

    {
        static constexpr const char* labels[DeviceBackendCount] = { "Intiface", "Serial", "Wifi" };
        static constexpr const char* tips[DeviceBackendCount] = {
            "Through Intiface Central, which holds the bluetooth connection.",
            "TCode down a serial port: a machine on USB, or one paired over Bluetooth.",
            "TCode over the network, to a machine on wifi.",
        };
        ImGui::BeginDisabled(connected || currentStatus == Status::Connecting);
        const int32_t picked = OFS::SegmentedControl("##DeviceBackend", labels, tips,
            DeviceBackendCount, state.backend);
        if (picked >= 0 && picked != state.backend) {
            state.backend = picked;
            std::lock_guard<std::mutex> lock(mutex);
            message.clear();
        }
        ImGui::EndDisabled();
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(connected || currentStatus == Status::Connecting);
    if (state.backend == DeviceBackendIntiface) drawIntifaceSettings();
    else drawTCodeSettings();
    ImGui::EndDisabled();

    ImGui::Spacing();
    if (connected) {
        if (ImGui::Button("Disconnect", ImVec2(-1.f, 0.f))) Disconnect();
        if (state.backend == DeviceBackendIntiface
            && ImGui::Button("Look for devices", ImVec2(-1.f, 0.f))) {
            StartScanning();
        }
    }
    else if (ImGui::Button("Connect", ImVec2(-1.f, 0.f))) {
        if (state.safetyNoteAccepted) Connect();
        else ImGui::OpenPopup(SafetyNoteId);
    }
    drawSafetyNote();

    if (!currentMessage.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        if (currentStatus == Status::Failed) ImGui::TextUnformatted(currentMessage.c_str());
        else ImGui::TextDisabled("%s", currentMessage.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (state.backend == DeviceBackendIntiface) {
        ImGui::TextDisabled("Devices");
        if (currentDevices.empty()) {
            ImGui::TextDisabled(connected
                ? "None yet. Look for devices, or pair one in Intiface."
                : "Not connected.");
        }
        for (size_t i = 0; i < currentDevices.size(); i += 1) {
            ImGui::PushID((int32_t)i);
            bool enabled = currentDevices[i].enabled;
            ImGui::BeginDisabled(!currentDevices[i].canMove);
            if (ImGui::Checkbox(currentDevices[i].name.c_str(), &enabled)) {
                std::lock_guard<std::mutex> lock(mutex);
                for (auto& device : devices) {
                    if (device.index == currentDevices[i].index) device.enabled = enabled;
                }
            }
            ImGui::EndDisabled();
            if (!currentDevices[i].canMove) {
                ImGui::SameLine();
                ImGui::TextDisabled("(cannot be told a position)");
            }
            ImGui::PopID();
        }

        ImGui::Spacing();
        OFS::StepperInt("Send ahead by (ms)", "##DeviceLatency", &state.latencyMs, 10, 0, 1000);
        OFS::Tooltip("How far ahead of the stroke the device is told, to cover what the link and "
                     "the hardware take. Raise it if the device lags behind the script.");
        OFS::StepperInt("Shallowest (%)", "##DeviceRangeMin", &state.rangeMin, 5, 0, 100);
        OFS::StepperInt("Deepest (%)", "##DeviceRangeMax", &state.rangeMax, 5, 0, 100);
        OFS::Tooltip("The part of the device's travel the script is mapped into.");
    }
    else {
        ImGui::TextDisabled("Axes");
        drawAxisTable();

        ImGui::Spacing();
        ImGui::TextDisabled("Easing home");
        OFS::StepperFloat("After (s)", "##AutoHomeDelay", &state.autoHomeDelaySeconds, 0.5f, 0.f, 30.f, "%.1f");
        OFS::Tooltip("How long an axis waits, with nothing driving it, before it goes back to its "
                     "rest position. Nothing drives an axis while the playhead is outside its "
                     "script, or while playback is paused.");
        OFS::StepperFloat("Over (s)", "##AutoHomeDuration", &state.autoHomeDurationSeconds, 0.1f, 0.05f, 10.f, "%.2f");
        OFS::Tooltip("How long it takes getting there.");
    }

    ImGui::Spacing();
    ImGui::Checkbox("Follow while paused", &state.followWhilePaused);
    OFS::Tooltip("Moves the device as you scrub, instead of only while playing.");

    ImGui::Spacing();
    ImGui::BeginDisabled(!connected);
    if (ImGui::Button("Stop the devices", ImVec2(-1.f, 0.f))) StopDevices();
    ImGui::EndDisabled();
    OFS::Tooltip("Everything back to its rest position.");

    if (connected) {
        if (state.backend == DeviceBackendIntiface) {
            ImGui::TextDisabled("%d sent", currentMoves);
        }
        else {
            // The rate asked for and the rate reached, since the second is
            // what the machine feels and the two are not the same.
            ImGui::TextDisabled("%d lines sent, %.0f a second", currentMoves, sendsPerSecond);
            OFS::Tooltip("Lines go out from the frame loop, so the interval above is a floor: "
                         "the app cannot send more often than it draws.");
        }
    }

    ImGui::End();
}

// ================================ TCode ====================================

// The axes a TCode machine has, and the script that drives each. The names are
// the ones OFS gives an axis script, and the channels are the ones the
// firmware answers to; the pairing is the same one the funscript 2.0 export
// writes, so a machine and a file agree about which axis is which.
static const struct { const char* channel; const char* script; int32_t rest; } DefaultAxes[] = {
    { "L0", "stroke", 50 },
    { "L1", "surge", 50 },
    { "L2", "sway", 50 },
    { "R0", "twist", 50 },
    { "R1", "roll", 50 },
    { "R2", "pitch", 50 },
    { "A1", "suck", 50 },
};

void OFS_DeviceLink::ensureAxes() noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);
    if (state.axes.size() == (sizeof(DefaultAxes) / sizeof(DefaultAxes[0]))) return;

    // Kept in the saved settings so the limits survive a restart, but rebuilt
    // whenever the list itself is not what this version of OFS knows about.
    state.axes.clear();
    for (const auto& axis : DefaultAxes) {
        DeviceAxisSettings settings;
        settings.channel = axis.channel;
        settings.script = axis.script;
        settings.restPosition = axis.rest;
        // The stroke axis is the one a project always has; the rest are only
        // driven once a script for them exists.
        settings.enabled = true;
        state.axes.push_back(settings);
    }
    axisState.assign(state.axes.size(), AxisRuntime());
}

// The script driving an axis, or nothing. An axis script is named for its axis
// - "name.roll.funscript" - which is the same naming the simulator reads.
static std::shared_ptr<Funscript> scriptForAxis(const std::string& wanted) noexcept
{
    auto app = OpenFunscripter::ptr;
    for (auto& script : app->LoadedFunscripts()) {
        if (script->AxisName() == wanted) return script;
        // The first script of a project is the stroke whether or not it says so.
        if (wanted == "stroke" && script == app->LoadedFunscripts().front()) return script;
    }
    return nullptr;
}

void OFS_DeviceLink::sendTCode(float deltaTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& state = DeviceLinkState::State(stateHandle);
    ensureAxes();

    sinceSend += deltaTime;
    const float interval = (float)Util::Clamp(state.sendIntervalMs, 3, 100) / 1000.f;
    if (sinceSend < interval) return;
    const float elapsed = sinceSend;
    sinceSend = 0.f;

    rateWindow += elapsed;
    rateCount += 1;
    if (rateWindow >= 0.5f) {
        sendsPerSecond = (float)rateCount / rateWindow;
        rateWindow = 0.f;
        rateCount = 0;
    }

    const float now = (float)app->player->CurrentTime();
    const bool playing = !app->player->IsPaused();

    char line[256];
    int32_t used = 0;
    for (size_t i = 0; i < state.axes.size() && i < axisState.size(); i += 1) {
        auto& settings = state.axes[i];
        auto& runtime = axisState[i];
        if (!settings.enabled) continue;

        // What the script asks for, 0..1 of the axis, and whether anything is
        // asking at all.
        bool driven = false;
        float wanted = (float)settings.restPosition / 100.f;
        if (auto script = scriptForAxis(settings.script)) {
            if (!script->Actions().empty()) {
                const float first = script->Actions().front().atS;
                const float last = script->Actions().back().atS;
                // Outside the scripted stretch there is nothing to follow, and
                // holding the last position there is what auto home is for.
                if (now >= first && now <= last) {
                    wanted = Util::Clamp(script->GetPositionAtTime(now) / 100.f, 0.f, 1.f);
                    driven = playing || state.followWhilePaused;
                }
            }
        }

        if (settings.invert) wanted = 1.f - wanted;

        // Auto home: an axis nothing is driving eases back to its rest
        // position rather than stopping wherever the script left it, which on
        // a machine means a limb held at an angle until someone turns it off.
        if (!driven && settings.autoHome) {
            runtime.idleTime += elapsed;
            const float delay = Util::Max(0.f, state.autoHomeDelaySeconds);
            const float duration = Util::Max(0.01f, state.autoHomeDurationSeconds);
            const float rest = settings.invert
                ? 1.f - ((float)settings.restPosition / 100.f)
                : (float)settings.restPosition / 100.f;
            if (runtime.idleTime <= delay) {
                wanted = std::isfinite(runtime.value) ? runtime.value : rest;
            }
            else {
                if (!std::isfinite(runtime.homeFrom)) {
                    runtime.homeFrom = std::isfinite(runtime.value) ? runtime.value : rest;
                }
                const float t = Util::Clamp((runtime.idleTime - delay) / duration, 0.f, 1.f);
                // Smoothstep, so it leaves and arrives gently rather than
                // starting with a jerk.
                const float eased = t * t * (3.f - (2.f * t));
                wanted = runtime.homeFrom + ((rest - runtime.homeFrom) * eased);
            }
        }
        else {
            runtime.idleTime = 0.f;
            runtime.homeFrom = std::numeric_limits<float>::quiet_NaN();
        }

        // The part of the machine's travel this axis is allowed to use.
        const float low = (float)Util::Min(settings.rangeMin, settings.rangeMax) / 100.f;
        const float high = (float)Util::Max(settings.rangeMin, settings.rangeMax) / 100.f;
        float value = low + ((high - low) * Util::Clamp(wanted, 0.f, 1.f));

        // Speed limit, in units of travel a second. Held to what the machine
        // can do rather than asking for a move it can only answer with a bang.
        if (settings.speedLimit > 0 && std::isfinite(runtime.value)) {
            const float step = value - runtime.value;
            const float reach = ((float)settings.speedLimit / 100.f) * elapsed;
            if (std::abs(step) > reach) {
                value = runtime.value + (step > 0.f ? reach : -reach);
            }
        }

        if (!OFS_TCode::IsDirty(value, runtime.sentValue)) {
            runtime.value = value;
            continue;
        }

        char command[32];
        const int32_t length = OFS_TCode::FormatAxis(command, sizeof(command),
            settings.channel.c_str(), value, (int32_t)std::lround(elapsed * 1000.f));
        if (length <= 0) continue;
        if (used + length + 2 >= (int32_t)sizeof(line)) break;
        if (used > 0) line[used++] = ' ';
        memcpy(line + used, command, (size_t)length);
        used += length;

        // Counted as having moved only once it has a value to move from: the
        // first line carries every axis, which says nothing about what is
        // driving them.
        const bool moved = std::isfinite(runtime.sentValue);
        runtime.value = value;
        runtime.sentValue = value;
        if (moved) {
            std::lock_guard<std::mutex> lock(mutex);
            channelsSent.insert(settings.channel);
        }
    }

    if (used == 0) return;
    line[used++] = '\n';
    if (!link.Write(line, (size_t)used)) {
        std::lock_guard<std::mutex> lock(mutex);
        status = Status::Failed;
        message = link.Error().empty() ? std::string("The device stopped listening.") : link.Error();
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    movesSent += 1;
}

// Puts every axis back to its rest position in one move, for disconnecting and
// for the stop button. Left where it was, a machine holds that position.
std::string OFS_DeviceLink::ChannelsSent() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex);
    std::string joined;
    for (const auto& channel : channelsSent) {
        if (!joined.empty()) joined += ",";
        joined += channel;
    }
    return joined;
}

std::string OFS_DeviceLink::AxisReport() const noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);
    std::string report;
    for (size_t i = 0; i < state.axes.size() && i < axisState.size(); i += 1) {
        if (!std::isfinite(axisState[i].sentValue)) continue;
        char entry[32];
        stbsp_snprintf(entry, sizeof(entry), "%s:%d", state.axes[i].channel.c_str(),
            (int32_t)std::lround(axisState[i].sentValue * (float)OFS_TCode::OutputMaximum));
        if (!report.empty()) report += ",";
        report += entry;
    }
    return report;
}

int32_t OFS_DeviceLink::StrokeValue() const noexcept
{
    if (axisState.empty() || !std::isfinite(axisState[0].sentValue)) return -1;
    return (int32_t)std::lround(axisState[0].sentValue * (float)OFS_TCode::OutputMaximum);
}

bool OFS_DeviceLink::AllAxesHome() const noexcept
{
    auto& state = DeviceLinkState::State(stateHandle);
    bool any = false;
    for (size_t i = 0; i < state.axes.size() && i < axisState.size(); i += 1) {
        if (!state.axes[i].enabled) continue;
        if (!std::isfinite(axisState[i].sentValue)) return false;
        const auto& settings = state.axes[i];
        float rest = (float)settings.restPosition / 100.f;
        if (settings.invert) rest = 1.f - rest;
        const float low = (float)Util::Min(settings.rangeMin, settings.rangeMax) / 100.f;
        const float high = (float)Util::Max(settings.rangeMin, settings.rangeMax) / 100.f;
        const float restMapped = low + ((high - low) * rest);
        if (OFS_TCode::IsDirty(restMapped, axisState[i].sentValue)) return false;
        any = true;
    }
    return any;
}

void OFS_DeviceLink::sendTCodeHome() noexcept
{
    if (!link.IsOpen()) return;
    auto& state = DeviceLinkState::State(stateHandle);
    ensureAxes();

    char line[256];
    int32_t used = 0;
    for (size_t i = 0; i < state.axes.size(); i += 1) {
        const auto& settings = state.axes[i];
        if (!settings.enabled) continue;
        float rest = (float)settings.restPosition / 100.f;
        if (settings.invert) rest = 1.f - rest;
        const float low = (float)Util::Min(settings.rangeMin, settings.rangeMax) / 100.f;
        const float high = (float)Util::Max(settings.rangeMin, settings.rangeMax) / 100.f;
        const float value = low + ((high - low) * rest);

        char command[32];
        const int32_t length = OFS_TCode::FormatAxis(command, sizeof(command),
            settings.channel.c_str(), value, 1000);
        if (used + length + 2 >= (int32_t)sizeof(line)) break;
        if (used > 0) line[used++] = ' ';
        memcpy(line + used, command, (size_t)length);
        used += length;
        if (i < axisState.size()) {
            axisState[i].sentValue = value;
            axisState[i].value = value;
        }
    }
    if (used == 0) return;
    line[used++] = '\n';
    link.Write(line, (size_t)used);
}
