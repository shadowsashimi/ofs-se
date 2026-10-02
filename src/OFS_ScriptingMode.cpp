#include "OpenFunscripter.h"
#include "OFS_ScriptingMode.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"
#include "OFS_Localization.h"

#include "imgui.h"
#include "imgui_internal.h"

#include "OFS_ImGui.h"
#include "OFS_SashimiTheme.h"

#include "state/ScriptModeState.h"


void ScriptingModeBase::AddEditAction(FunscriptAction action) noexcept
{
    auto app = OpenFunscripter::ptr;
    ctx().AddEditAction(action, app->scripting->LogicalFrameTime());
}

Funscript* ScriptingModeBase::TargetOverride = nullptr;

inline Funscript& ScriptingModeBase::ctx() noexcept
{
    if (TargetOverride != nullptr) return *TargetOverride;
    auto app = OpenFunscripter::ptr;
    return *app->ActiveFunscript().get();
}

void ScriptingMode::Init() noexcept
{
    stateHandle = OFS_AppState<ScriptingModeState>::Register(ScriptingModeState::StateName);

    modes[ScriptingModeEnum::DEFAULT_MODE] = std::make_unique<DefaultMode>();
    modes[ScriptingModeEnum::ALTERNATING] = std::make_unique<AlternatingMode>();
    modes[ScriptingModeEnum::RECORDING] = std::make_unique<RecordingMode>();
    modes[ScriptingModeEnum::DYNAMIC_INJECTION] = std::make_unique<DynamicInjectionMode>();
    SetMode(ScriptingModeEnum::DEFAULT_MODE);
    SetOverlay(ScriptingOverlayModes::FRAME);
}

// What each mode does to a point as you place it, and what the grid under the
// timeline is measuring. Both are laid out as a row of radio buttons rather
// than a dropdown: four options and three options respectively, so the one you
// want is readable without opening anything, and switching is one click rather
// than two.
//
// The tooltips carry the explanation the names cannot. "Dynamic injection" said
// nothing about placing a peak between two points, and a name alone was never
// going to.
struct ScriptingModeEntry
{
    ScriptingModeEnum mode;
    Tr label;
    const char* tip;
};

static const ScriptingModeEntry ScriptingModeEntries[] = {
    { ScriptingModeEnum::DEFAULT_MODE, Tr::DEFAULT_MODE,
        "Places each point exactly where you put it, and changes nothing about it." },
    { ScriptingModeEnum::ALTERNATING, Tr::ALTERNATING_MODE,
        "Sends every point to the opposite end from the one before it, so clicking "
        "repeatedly draws a stroke. Either between a fixed top and bottom, or reading "
        "the direction off the previous point." },
    { ScriptingModeEnum::DYNAMIC_INJECTION, Tr::DYNAMIC_INJECTION_MODE,
        "Adds a peak between your last point and the new one, turning single clicks "
        "into whole strokes. Target speed caps how far that peak can travel, and the "
        "offset slides it off centre." },
    { ScriptingModeEnum::RECORDING, Tr::RECORDING_MODE,
        "Captures movement live from the mouse or a controller while the video plays, "
        "instead of placing points one at a time." },
};

struct OverlayModeEntry
{
    ScriptingOverlayModes mode;
    Tr label;
    const char* tip;
};

// Ordered by how much grid there is, none first, rather than by the order the
// enum happens to declare them in.
static const OverlayModeEntry OverlayModeEntries[] = {
    { ScriptingOverlayModes::EMPTY, Tr::EMPTY_OVERLAY,
        "No grid. Points land wherever you put them." },
    { ScriptingOverlayModes::FRAME, Tr::FRAME_OVERLAY,
        "A line per frame of video, and snapping lands points on frame boundaries." },
    { ScriptingOverlayModes::TEMPO, Tr::TEMPO_OVERLAY,
        "A musical grid at the tempo of the chapter under the playhead, and snapping "
        "lands points on the beat." },
};


void ScriptingMode::DrawModeSelector(const char* id) noexcept
{
    constexpr int32_t count = (int32_t)IM_ARRAYSIZE(ScriptingModeEntries);
    const char* labels[count];
    const char* tips[count];
    int32_t current = -1;
    for (int32_t i = 0; i < count; i += 1) {
        labels[i] = TRD(ScriptingModeEntries[i].label);
        tips[i] = ScriptingModeEntries[i].tip;
        if (activeMode == ScriptingModeEntries[i].mode) current = i;
    }
    const int32_t picked = OFS::SegmentedControl(id, labels, tips, count, current);
    if (picked >= 0) SetMode(ScriptingModeEntries[picked].mode);
}

void ScriptingMode::DrawOverlaySelector(const char* id) noexcept
{
    constexpr int32_t count = (int32_t)IM_ARRAYSIZE(OverlayModeEntries);
    const char* labels[count];
    const char* tips[count];
    int32_t current = -1;
    for (int32_t i = 0; i < count; i += 1) {
        labels[i] = TRD(OverlayModeEntries[i].label);
        tips[i] = OverlayModeEntries[i].tip;
        if (activeOverlay == OverlayModeEntries[i].mode) current = i;
    }
    const int32_t picked = OFS::SegmentedControl(id, labels, tips, count, current);
    if (picked >= 0) SetOverlay(OverlayModeEntries[picked].mode);
}

void ScriptingMode::DrawScriptingMode(bool* open) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& state = ScriptingModeState::State(stateHandle);
    auto app = OpenFunscripter::ptr;
    ImGui::Begin(TR_ID(WindowId, Tr::MODE), open);
    ImGui::PushItemWidth(-1);

    // With the toolbar showing, the mode and grid are already picked there, a
    // row above, so this panel keeps only the settings that go with each
    // choice and names the choice it is showing settings for. The bars come
    // back when the toolbar is hidden.
    const bool pickedOnToolbar = app->ToolbarVisible();

    if (pickedOnToolbar) {
        for (auto& entry : ScriptingModeEntries) {
            if (entry.mode == activeMode) OFS::SeparatorText(TRD(entry.label));
        }
    }
    else {
        DrawModeSelector("##Mode");
    }
    Mode()->DrawModeSettings();

    ImGui::Spacing();
    if (pickedOnToolbar) {
        for (auto& entry : OverlayModeEntries) {
            if (entry.mode == activeOverlay) OFS::SeparatorText(FMT("%s grid", TRD(entry.label)));
        }
    }
    else {
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);
        ImGui::Spacing();
        DrawOverlaySelector("##OverlayMode");
    }
    DrawOverlaySettings();
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Horizontal);
    ImGui::Spacing();
    // Typed, with steps either side, and named on a line above it.
    OFS::StepperInt("Insert offset (ms)", "##InsertOffset", &state.actionInsertDelayMs, 10, -1000, 1000);
    OFS::Tooltip(TR(OFFSET_TOOLTIP));
    ImGui::End();
}

void ScriptingMode::DrawOverlaySettings() noexcept
{
    overlayImpl->DrawSettings();
}

void ScriptingMode::SetMode(ScriptingModeEnum mode) noexcept
{
    Mode()->Finish();
    if (mode >= ScriptingModeEnum::DEFAULT_MODE && mode < ScriptingModeEnum::COUNT) {
        activeMode = mode;
    }
    else {
        activeMode = ScriptingModeEnum::DEFAULT_MODE;
    }
}

void ScriptingMode::SetOverlay(ScriptingOverlayModes mode) noexcept
{
    activeOverlay = mode;
    auto app = OpenFunscripter::ptr;
    auto timeline = &app->scriptTimeline;
    switch (mode) {
        case ScriptingOverlayModes::FRAME:
            overlayImpl = std::make_unique<FrameOverlay>(timeline);
            break;
        case ScriptingOverlayModes::TEMPO:
            overlayImpl = std::make_unique<TempoOverlay>(timeline);
            break;
        case ScriptingOverlayModes::EMPTY:
            overlayImpl = std::make_unique<EmptyOverlay>(timeline);
            break;
        default:
            break;
    }
}

void ScriptingMode::Undo() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    Mode()->Undo();
}

void ScriptingMode::Redo() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    Mode()->Redo();
}

void ScriptingMode::AddEditAction(FunscriptAction action) noexcept
{
    auto app = OpenFunscripter::ptr;
    if (!app->player->IsPaused()) {
        // apply offset
        auto& state = ScriptingModeState::State(stateHandle);
        action.atS += state.actionInsertDelayMs / 1000.f;
    }
    Mode()->AddEditAction(action);
}

void ScriptingMode::NextFrame() noexcept
{
    auto app = OpenFunscripter::ptr;
    float frameTime = app->player->FrameTime();
    overlayImpl->nextFrame(frameTime);
}

void ScriptingMode::PreviousFrame() noexcept
{
    auto app = OpenFunscripter::ptr;
    float frameTime = app->player->FrameTime();
    overlayImpl->previousFrame(frameTime);
}

float ScriptingMode::SteppingIntervalForward(float fromTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    float frameTime = app->player->FrameTime();
    return overlayImpl->steppingIntervalForward(frameTime, fromTime);
}

float ScriptingMode::SteppingIntervalBackward(float fromTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    float frameTime = app->player->FrameTime();
    return overlayImpl->steppingIntervalBackward(frameTime, fromTime);
}

float ScriptingMode::LogicalFrameTime() noexcept
{
    auto app = OpenFunscripter::ptr;
    float realFrameTime = app->player->FrameTime();
    return overlayImpl->logicalFrameTime(realFrameTime);
}

void ScriptingMode::Update() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    Mode()->Update();
    overlayImpl->update();
}

// dynamic top injection
void DynamicInjectionMode::DrawModeSettings() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // Typed, with a step either side: parameters are numbers to set exactly,
    // where a slider hid the value and made a round one hard to land on.
    if (OFS::StepperFloat("Target speed (units/s)", "##TargetSpeed", &targetSpeed, 50.f, MinSpeed, MaxSpeed, "%.0f")) {
        targetSpeed = std::round(targetSpeed);
    }
    OFS::Tooltip(TR(DI_TARGET_SPEED));

    OFS::StepperFloat("Peak offset", "##PeakOffset", &peakOffset, 0.1f, -0.9f, 0.9f, "%+.2f");
    OFS::Tooltip(TR(DI_PEAK_OFFSET));

    const char* directionLabels[2] = { TR(TOP), TR(BOTTOM) };
    static constexpr const char* directionTips[2] = {
        "The added point goes to the top, so each click draws a stroke up and back down.",
        "The added point goes to the bottom, so each click draws a stroke down and back up.",
    };
    const int32_t picked = OFS::SegmentedControl("##InjectDirection", directionLabels, directionTips,
        2, topBottomDirection == 1 ? 0 : 1);
    if (picked >= 0) topBottomDirection = picked == 0 ? 1 : -1;
}

// dynamic injection
void DynamicInjectionMode::AddEditAction(FunscriptAction action) noexcept
{
    auto previous = ctx().GetPreviousActionBehind(action.atS);
    if (previous != nullptr && action.atS > previous->atS) {
        const float gap = action.atS - previous->atS;
        const float half = gap / 2.f;
        const float injectAt = previous->atS + half + (half * peakOffset);
        const float leadDuration = injectAt - previous->atS;
        const float trailDuration = action.atS - injectAt;

        // targetSpeed governs both halves of the stroke, not just the first.
        // Deriving the peak from the previous action alone, as this used to,
        // pinned the lead half to targetSpeed and left the return half to run
        // at targetSpeed * (1 + offset) / (1 - offset) -- nineteen times over
        // at the far end of the offset range.
        //
        // Each neighbour bounds the peak on both sides instead: it can sit at
        // most duration * targetSpeed above or below that action. Intersecting
        // the two windows leaves every position reachable without either half
        // breaking the limit.
        const float leadReach = leadDuration * targetSpeed;
        const float trailReach = trailDuration * targetSpeed;
        const float lowest = std::max(previous->pos - leadReach, action.pos - trailReach);
        const float highest = std::min(previous->pos + leadReach, action.pos + trailReach);

        float peak;
        if (lowest <= highest) {
            // Take the window's far end, so the peak still travels as far as
            // the limit permits rather than settling for less.
            peak = topBottomDirection > 0 ? highest : lowest;
        }
        else {
            // The window is empty: these two actions are already further apart
            // than targetSpeed covers, and no point between them can obey it.
            // The straight line is the least bad place to sit, leaving both
            // halves at exactly the speed the two actions already demanded.
            // The injection cannot make that right, but it must not make it
            // worse by adding a detour on top.
            peak = previous->pos + ((action.pos - previous->pos) * (leadDuration / gap));
        }

        // Clamping to the position range only ever pulls the peak towards its
        // neighbours, and both of those already sit inside the range, so it
        // cannot push either half back over the speed limit.
        auto injectPos = Util::Clamp<int32_t>(std::round(peak), 0, 100);
        ScriptingModeBase::AddEditAction(FunscriptAction(injectAt, injectPos));
    }
    ScriptingModeBase::AddEditAction(action);
}

// alternating
void AlternatingMode::DrawModeSettings() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    if (contextSensitive) {
        auto behind = ctx().GetPreviousActionBehind(std::round(app->player->CurrentTime()) - 0.001f);
        if (behind) {
            ImGui::TextDisabled("%s: %s", TR(NEXT_POINT), behind->pos <= 50 ? TR(TOP) : TR(BOTTOM));
        }
        else {
            ImGui::TextDisabled("%s: %s", TR(NEXT_POINT), TR(BOTTOM));
        }
    }
    else {
        if (fixedRangeEnabled) {
            ImGui::TextDisabled(TR(NEXT_POINT_AT_FMT), nextPosition ? fixedBottom : fixedTop);
        }
        else {
            ImGui::TextDisabled(TR(NEXT_POINT_IS_FMT), nextPosition ? TR(INVERTED) : TR(NOT_INVERTED));
        }
    }
    ImGui::Checkbox(TR(FIXED_RANGE), &fixedRangeEnabled);
    ImGui::Checkbox(TR(CONTEXT_SENSITIVE), &contextSensitive);
    OFS::Tooltip(TR(CONTEXT_SENSITIVE_TOOLTIP));
    if (fixedRangeEnabled) {
        OFS::StepperInt("Fixed bottom", "##FixedBottom", &fixedBottom, 5, 0, 100);
        OFS::StepperInt("Fixed top", "##FixedTop", &fixedTop, 5, 0, 100);
        const bool inputActive = ImGui::IsAnyItemActive();

        if (fixedBottom > fixedTop && !inputActive) {
            // correct user error :^)
            auto tmp = fixedBottom;
            fixedBottom = fixedTop;
            fixedTop = tmp;
        }
    }
}

void AlternatingMode::AddEditAction(FunscriptAction action) noexcept
{
    if (contextSensitive) {
        auto behind = ctx().GetPreviousActionBehind(action.atS - 0.001f);
        if (behind && behind->pos <= 50 && action.pos <= 50) {
            // Top
            action.pos = 100 - action.pos;
        }
        else if (behind && behind->pos > 50 && action.pos > 50) {
            // Bottom
            action.pos = 100 - action.pos;
        }
    }
    else {
        if (fixedRangeEnabled) {
            action.pos = nextPosition ? fixedBottom : fixedTop;
        }
        else {
            action.pos = nextPosition ? 100 - action.pos : action.pos;
        }
    }
    ScriptingModeBase::AddEditAction(action);
    if (!contextSensitive) {
        nextPosition = !nextPosition;
    }
}

void AlternatingMode::Undo() noexcept
{
    nextPosition = !nextPosition;
}

void AlternatingMode::Redo() noexcept
{
    nextPosition = !nextPosition;
}

inline void RecordingMode::singleAxisRecording() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    recordingAxisX->AddAction(FunscriptAction(app->player->CurrentTime(), currentPosY));
    app->simulator.positionOverride = currentPosY;
}

inline void RecordingMode::twoAxisRecording() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;

    float atS = app->player->CurrentTime();
    recordingAxisX->AddAction(FunscriptAction(atS, currentPosX));
    recordingAxisY->AddAction(FunscriptAction(atS, currentPosY));
}

// recording
RecordingMode::RecordingMode() noexcept
{
    eventUnsub = EV::MakeUnsubscibeFn(SDL_CONTROLLERAXISMOTION,
        EV::Queue().appendListener(SDL_CONTROLLERAXISMOTION,
            OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &RecordingMode::ControllerAxisMotion))));
}

RecordingMode::~RecordingMode() noexcept
{
    eventUnsub();
}

void RecordingMode::ControllerAxisMotion(const OFS_SDL_Event* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if (activeType != RecordingType::Controller) return;
    auto& axis = ev->sdl.caxis;
    const float range = (float)std::numeric_limits<int16_t>::max() - ControllerDeadzone;
    int16_t axisValue = axis.value;

    if (axis.value >= 0 && axis.value < ControllerDeadzone)
        axisValue = 0;
    else if (axis.value < 0 && axis.value > -ControllerDeadzone)
        axisValue = 0;
    else if (axis.value >= ControllerDeadzone)
        axisValue -= ControllerDeadzone;
    else if (axis.value <= ControllerDeadzone)
        axisValue += ControllerDeadzone;

    switch (axis.axis) {
        case SDL_CONTROLLER_AXIS_LEFTX:
            leftX = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
        case SDL_CONTROLLER_AXIS_LEFTY:
            leftY = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
        case SDL_CONTROLLER_AXIS_RIGHTX:
            rightX = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
        case SDL_CONTROLLER_AXIS_RIGHTY:
            rightY = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            leftTrigger = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            rightTrigger = Util::Clamp(axisValue / range, -1.f, 1.f);
            break;
    }


    if (std::abs(rightX) > std::abs(leftX)) {
        valueX = rightX;
    }
    else {
        valueX = leftX;
    }

    if (std::abs(rightY) > std::abs(leftY)) {
        valueY = -rightY;
    }
    else {
        valueY = -leftY;
    }
}

inline static const char* RecordingModeToString(RecordingMode::RecordingType mode) noexcept
{
    switch (mode) {
        case RecordingMode::RecordingType::Controller: return TR(CONTROLLER);
        case RecordingMode::RecordingType::Mouse: return TR(MOUSE);
    }
    return "";
}

void RecordingMode::DrawModeSettings() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;

    // Two options, both visible, one click to switch: the same segmented bar
    // as the modes above it, where this was a dropdown.
    {
        const char* labels[2] = { TR(MOUSE), TR(CONTROLLER) };
        static constexpr const char* tips[2] = {
            "Records the position from the mouse, moved up and down over the simulator.",
            "Records the position from a game controller's stick while the video plays.",
        };
        const int32_t picked = OFS::SegmentedControl("##RecordingInput", labels, tips, 2,
            activeType == RecordingType::Mouse ? 0 : 1);
        if (picked >= 0) activeType = picked == 0 ? RecordingType::Mouse : RecordingType::Controller;
    }

    switch (activeType) {
        case RecordingType::Controller: {
            OFS::StepperInt("Deadzone", "##Deadzone", &ControllerDeadzone, 1000, 0, std::numeric_limits<int16_t>::max());
            OFS::Tooltip(TR(CONTROLLER_DEADZONE));
            ImGui::Checkbox(TR(CENTER), &controllerCenter);
            if (controllerCenter) {
                currentPosX = Util::Clamp<int32_t>(50.f + (50.f * valueX), 0, 100);
                currentPosY = Util::Clamp<int32_t>(50.f + (50.f * valueY), 0, 100);
            }
            else {
                currentPosX = Util::Clamp<int32_t>(100.f * std::abs(valueX), 0, 100);
                currentPosY = Util::Clamp<int32_t>(100.f * std::abs(valueY), 0, 100);
            }
            if (!recordingActive) {
                ImGui::SameLine();
                ImGui::Checkbox(TR(TWO_AXES), &twoAxesMode);
                OFS::Tooltip(TR(TWO_AXES_TOOLTIP));
            }
            break;
        }
        case RecordingType::Mouse: {
            twoAxesMode = false;
            valueY = app->simulator.getMouseValue();
            currentPosY = Util::Clamp<int32_t>(50.f + (50.f * valueY), 0, 100);
            break;
        }
    }

    ImGui::Checkbox(TR(INVERT), &inverted);
    ImGui::SameLine();
    ImGui::Checkbox(TR(RECORD_ON_PLAY), &automaticRecording);
    if (inverted) {
        currentPosX = 100 - currentPosX;
        currentPosY = 100 - currentPosY;
    }
    if (twoAxesMode) {
        ImGui::TextUnformatted(TR(TWO_AXES_AXES));
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::SliderInt("##PosX", &currentPosX, 0, 100);
        ImGui::SliderInt("##PosY", &currentPosY, 0, 100);
        ImGui::PopItemFlag();
    }
    else {
        ImGui::TextUnformatted(TR(POSITION));
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        ImGui::SliderInt("##Pos", &currentPosY, 0, 100);
        ImGui::PopItemFlag();
    }


    ImGui::Spacing();
    bool playing = !app->player->IsPaused();
    if (automaticRecording && playing && recordingActive != playing) {
        if (!twoAxesMode) {
            recordingAxisX = app->ActiveFunscript();
            app->undoSystem->Snapshot(StateType::GENERATE_ACTIONS, recordingAxisX);
            recordingActive = true;
        }
        else {
            recordingAxisX = nullptr;
            recordingAxisY = nullptr;
            for (auto& script : app->LoadedFunscripts()) {
                if (script->AxisName() == "roll") {
                    recordingAxisX = script;
                }
                else if (script->AxisName() == "pitch") {
                    recordingAxisY = script;
                }
            }

            if (recordingAxisX == nullptr || recordingAxisY == nullptr) {
                twoAxesMode = false;
            }
            else {
                app->undoSystem->Snapshot(StateType::GENERATE_ACTIONS, { recordingAxisX, recordingAxisY });
                recordingActive = true;
            }
        }
    }
    else if (!playing && recordingActive) {
        recordingAxisX = nullptr;
        recordingAxisY = nullptr;
        recordingActive = false;
    }

    if (recordingActive && playing) {
        ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::PinkBright);
        ImGui::TextUnformatted(TR(RECORDING_ACTIVE));
        ImGui::PopStyleColor();
    }
    else {
        ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::Grey60);
        ImGui::TextUnformatted(TR(RECORDING_PAUSED));
        ImGui::PopStyleColor();
    }
}

void RecordingMode::Update() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    if (recordingActive) {
        if (twoAxesMode) {
            twoAxisRecording();
        }
        else {
            singleAxisRecording();
        }
    }
}

void RecordingMode::Finish() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // this fixes a bug when the mode gets changed during a recording
    if (recordingActive) {
        recordingAxisX = nullptr;
        recordingAxisY = nullptr;
        recordingActive = false;
    }
}
