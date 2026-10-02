#include "OpenFunscripter.h"
#include "OFS_SpecialFunctions.h"
#include "FunscriptUndoSystem.h"
#include "OFS_ImGui.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "OFS_Lua.h"

#include <vector>
#include <sstream>

#include "state/SpecialFunctionsState.h"
#include "state/ProjectState.h"
#include "state/states/ChapterState.h"
#include "OFS_ChapterManager.h"
#include "OFS_ScriptPositionsOverlays.h"
#include "stb_sprintf.h"

#include <algorithm>

#include "SDL_thread.h"
#include "SDL_atomic.h"

#include <cmath>

SpecialFunctionsWindow::SpecialFunctionsWindow() noexcept
{
    stateHandle = OFS_AppState<SpecialFunctionState>::Register(SpecialFunctionState::StateName);
    auto& state = SpecialFunctionState::State(stateHandle);
    SetFunction(state.selectedFunction);
}

void SpecialFunctionsWindow::SetFunction(SpecialFunctionType functionEnum) noexcept
{
    if (function != nullptr) {
        function->Hidden();
        delete function; function = nullptr;
    }
    auto& state = SpecialFunctionState::State(stateHandle);

	switch (functionEnum) {
        case SpecialFunctionType::RangeExtender:
            function = new FunctionRangeExtender();
            break;
        case SpecialFunctionType::RamerDouglasPeucker:
            function = new RamerDouglasPeucker();
            break;
        case SpecialFunctionType::MultiAxis:
            function = new MultiAxisGenerator(stateHandle);
            break;
        case SpecialFunctionType::BeatFill:
            function = new BeatFill(stateHandle);
            break;
        case SpecialFunctionType::Loudness:
            function = new LoudnessDepth(stateHandle);
            break;
        default:
            function = new FunctionRangeExtender();
            functionEnum = SpecialFunctionType::RangeExtender;
            break;
	}
    state.selectedFunction = functionEnum;
}

void SpecialFunctionsWindow::ShowFunctionsWindow(bool* open) noexcept
{
	if (open != nullptr && !(*open)) {
        if (function != nullptr) function->Hidden();
        return;
    }
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
	// A size and place for the first time it opens. The default layout has no
	// slot for this panel, and left to size itself it opened a few dozen
	// pixels wide with its explanation wrapping one letter to a line down the
	// height of the screen. After that, wherever the user puts it is kept.
	{
		const auto* viewport = ImGui::GetMainViewport();
		const float em = ImGui::GetFontSize();
		// Tall enough for the longest tool, which is the beat fill: opened at the
		// height the two short tools needed, its button sat below the bottom
		// edge with nothing to say a scroll was necessary.
		ImGui::SetNextWindowSize(ImVec2(em * 24.f, em * 30.f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	}
	ImGui::Begin(TR_ID(WindowId, Tr::SPECIAL_FUNCTIONS), open, ImGuiWindowFlags_None);
	ImGui::SetNextItemWidth(-1.f);

    // Both tools showing and one click apart, the same segmented bar as the
    // modes use, where this was a dropdown that hid the other tool's name.
    auto& state = SpecialFunctionState::State(stateHandle);
    {
        static constexpr const char* labels[5] = {
            "Range extend", "Simplify", "Multi-axis", "Beat fill", "Loudness"
        };
        static constexpr const char* tips[5] = {
            "Pushes the top of every stroke in a selection up and its bottom down.",
            "Removes the points in a selection that add least to its shape (Ramer-Douglas-Peucker).",
            "Writes twist, roll and pitch that move with the selected stroke points.",
            "Writes strokes along the beat, across a chapter or the whole mix.",
            "Scales the selected strokes by how loud the audio is under them.",
        };
        const int32_t current = (int32_t)state.selectedFunction;
        const int32_t picked = OFS::SegmentedControl("##Functions", labels, tips, 5, current);
        if(picked >= 0 && picked != current) {
            SetFunction((SpecialFunctionType)picked);
        }
    }

	ImGui::Spacing();
	function->DrawUI();
	ImGui::End();
}

inline Funscript& FunctionBase::ctx() noexcept
{
    auto app = OpenFunscripter::ptr;
	return *app->ActiveFunscript().get();
}

// range extender
FunctionRangeExtender::FunctionRangeExtender() noexcept
{
    //auto app = OpenFunscripter::ptr;
    eventUnsub = EV::MakeUnsubscibeFn(FunscriptSelectionChangedEvent::EventType,
        EV::Queue().appendListener(FunscriptSelectionChangedEvent::EventType, 
           FunscriptSelectionChangedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &FunctionRangeExtender::SelectionChanged))));
}

FunctionRangeExtender::~FunctionRangeExtender() noexcept
{
    eventUnsub();
}

void FunctionRangeExtender::SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    if (!app->ActiveFunscript()->Selection().empty()) {
        rangeExtend = 0;
        createUndoState = true;
    }
}

void FunctionRangeExtender::DrawUI() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& undoSystem = app->ActiveFunscript()->undoSystem;
    if (app->ActiveFunscript()->SelectionSize() > 4 || (undoSystem->MatchUndoTop(StateType::RANGE_EXTEND))) {
        // Named for what it does, rather than as "Range" beside a bare number.
        const bool changed = OFS::StepperInt("Extend by", "##RangeExtend", &rangeExtend, 5, -50, 100);
        OFS::Tooltip("Positive pushes every stroke's top up and bottom down by this much, "
                     "negative pulls them in towards the middle.");
        if (changed) {
            rangeExtend = Util::Clamp<int32_t>(rangeExtend, -50, 100);
            if (createUndoState || 
                !undoSystem->MatchUndoTop(StateType::RANGE_EXTEND)) {
                app->undoSystem->Snapshot(StateType::RANGE_EXTEND, app->ActiveFunscript());
            }
            else {
                app->Undo();
                app->undoSystem->Snapshot(StateType::RANGE_EXTEND, app->ActiveFunscript());
            }
            createUndoState = false;
            ctx().RangeExtendSelection(rangeExtend);
        }
    }
    else
    {
        // Says what the tool is for before saying why it is unavailable, since
        // a greyed out slider on its own explains neither.
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Pushes the top of every stroke in a selection up and its bottom "
                            "down by the same amount, keeping the timing. Negative values pull "
                            "them in towards the middle.");
        ImGui::Spacing();
        ImGui::TextDisabled("Select at least 5 points to use it. %u selected.",
            app->ActiveFunscript()->SelectionSize());
        ImGui::PopTextWrapPos();
    }
}

RamerDouglasPeucker::RamerDouglasPeucker() noexcept
{
    auto app = OpenFunscripter::ptr;
    eventUnsub = EV::MakeUnsubscibeFn(FunscriptSelectionChangedEvent::EventType, EV::Queue().appendListener(FunscriptSelectionChangedEvent::EventType,
        FunscriptSelectionChangedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &RamerDouglasPeucker::SelectionChanged))));
}

RamerDouglasPeucker::~RamerDouglasPeucker() noexcept
{
    eventUnsub();
}

void RamerDouglasPeucker::SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    // Simplifying selected the points it left; that is not a new selection,
    // and resetting here would snap the tolerance back to zero mid drag.
    if (ownSelectionChangesPending > 0) {
        ownSelectionChangesPending -= 1;
        return;
    }
    auto app = OpenFunscripter::ptr;
    if (!app->ActiveFunscript()->Selection().empty()) {
        epsilon = 0.f;
        createUndoState = true;
    }
}

inline static float PointLineDistance(FunscriptAction pt, FunscriptAction lineStart, FunscriptAction lineEnd) noexcept {
    float dx = lineEnd.atS - lineStart.atS;
    float dy = lineEnd.pos - lineStart.pos;

    // Normalize
    float mag = sqrtf(dx * dx + dy * dy);
    if (mag > 0.0f) {
        dx /= mag;
        dy /= mag;
    }
    float pvx = pt.atS - lineStart.atS;
    float pvy = pt.pos - lineStart.pos;

    // Get dot product (project pv onto normalized direction)
    float pvdot = dx * pvx + dy * pvy;

    // Scale line direction vector and subtract it from pv
    float ax = pvx - pvdot * dx;
    float ay = pvy - pvdot * dy;

    return sqrtf(ax * ax + ay * ay);
}

static std::vector<bool> DouglasPeucker(const FunscriptArray& points, int startIndex, int lastIndex, float epsilon) noexcept {
    OFS_PROFILE(__FUNCTION__);
    std::vector<std::pair<int, int>> stk;
    stk.push_back(std::make_pair(startIndex, lastIndex));
    
    int globalStartIndex = startIndex;
    auto bitArray = std::vector<bool>();
    bitArray.resize(lastIndex - startIndex + 1, true);

    while (!stk.empty()) {
        startIndex = stk.back().first;
        lastIndex = stk.back().second;
        stk.pop_back();

        float dmax = 0.f;
        int index = startIndex;

        for (int i = index + 1; i < lastIndex; ++i) {
            if (bitArray[i - globalStartIndex]) {
                float d = PointLineDistance(points[i], points[startIndex], points[lastIndex]);

                if (d > dmax) {
                    index = i;
                    dmax = d;
                }
            }
        }

        if (dmax > epsilon) {
            stk.push_back(std::make_pair(startIndex, index));
            stk.push_back(std::make_pair(index, lastIndex));
        }
        else {
            for (int i = startIndex + 1; i < lastIndex; ++i) {
                bitArray[i - globalStartIndex] = false;
            }
        }
    }

    return bitArray;
}

static void DouglasPeucker(const FunscriptArray& points, float epsilon, FunscriptArray& newActions) noexcept {
    OFS_PROFILE(__FUNCTION__);
    auto bitArray = DouglasPeucker(points, 0, points.size() - 1, epsilon);
    newActions.reserve(points.size());

    for (int i = 0, n = points.size(); i < n; ++i) {
        if (bitArray[i]) {
            // we can safely assume points to be sorted
            newActions.emplace_back_unsorted(points[i]);
        }
    }
}

void RamerDouglasPeucker::DrawUI() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    if (app->ActiveFunscript()->SelectionSize() > 4 || (app->ActiveFunscript()->undoSystem->MatchUndoTop(StateType::SIMPLIFY))) {
        // Named for what it does rather than for the variable in the
        // algorithm.
        const bool changed = OFS::StepperFloat("Tolerance", "##SimplifyTolerance", &epsilon, 0.05f, 0.f, 10.f, "%.3f");
        OFS::Tooltip("Raise it to remove more points. A point goes when the line between "
                     "its neighbours passes within this distance of it, scaled to how far "
                     "apart the selected points are.");
        if (changed) {
            epsilon = std::max(epsilon, 0.f);
            if (createUndoState ||
                !app->ActiveFunscript()->undoSystem->MatchUndoTop(StateType::SIMPLIFY)) {
                // Average distance between neighbouring points in the selection,
                // which scales epsilon. Starts from zero every time: it used to
                // carry the previous selection's total forward, so each new
                // selection simplified harder than the one before it.
                averageDistance = 0.f;
                int count = 0;
                for (int i = 0, size = ctx().Selection().size(); i < size - 1; ++i) {
                    auto action1 = ctx().Selection()[i];
                    auto action2 = ctx().Selection()[i + 1];
                    
                    float dx = action1.atS - action2.atS;
                    float dy = action1.pos - action2.pos;
                    float distance = sqrtf((dx * dx) + (dy * dy));
                    averageDistance += distance;
                    ++count;
                }
                // Reached with a single point when the slider is dragged again
                // on a selection that an earlier simplify already reduced.
                if (count > 0) averageDistance /= (float)count;
            }
            else {
                app->undoSystem->Undo();
            }
            app->undoSystem->Snapshot(StateType::SIMPLIFY, app->ActiveFunscript());

            createUndoState = false;
            auto selection = ctx().Selection();
            ctx().RemoveSelectedActions();
            FunscriptArray newActions;
            newActions.reserve(selection.size());
            float scaledEpsilon = epsilon * averageDistance;
            DouglasPeucker(selection, scaledEpsilon, newActions);
            ctx().AddMultipleActions(newActions);

            // Keep what is left selected, as range extend does, so the same
            // points can be simplified further or handed to another tool
            // without selecting them again. That is a selection change of
            // our own making, and SelectionChanged is told to let it pass:
            // left to reset the tolerance, it would snap the drag to zero.
            ownSelectionChangesPending += 1;
            ctx().SetSelection(newActions);
        }
    }
    else {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Removes the points in a selection that add least to its shape. "
                            "The higher epsilon goes, the more are removed.");
        ImGui::Spacing();
        ImGui::TextDisabled("Select at least 5 points to use it. %u selected.",
            app->ActiveFunscript()->SelectionSize());
        ImGui::PopTextWrapPos();
    }
}

// ---------------------------------------------------------------------------
// Multi-axis generator
//
// The stroke is read as a wave: every turn in the selected points, bottom or
// top, is half a turn of a phase angle, 0 at a bottom and pi at the top after
// it. Each axis is then a function of that angle, so whatever the timing of
// the strokes, twist, roll and pitch stay locked to them.
// ---------------------------------------------------------------------------

static constexpr const char* MultiAxisLabels[3] = { "twist", "roll", "pitch" };

// The channel a script is for, from its file name: name.roll.funscript is
// roll, and a name with no channel is the stroke. As the simulator reads it.
static std::string MultiAxisChannel(const Funscript& script) noexcept
{
    return script.AxisName();
}

static MultiAxisAxisSettings& MultiAxisSettingsFor(SpecialFunctionState& state, int32_t axis) noexcept
{
    switch (axis) {
        case 0: return state.twist;
        case 1: return state.roll;
        default: return state.pitch;
    }
}

struct StrokeTurn
{
    float atS;
    float theta;
    float pos;
};

// The first point, every point where the stroke changes direction, and the
// last point, each with its phase.
static std::vector<StrokeTurn> FindStrokeTurns(const FunscriptArray& points) noexcept
{
    std::vector<StrokeTurn> turns;
    if (points.size() < 2) return turns;

    std::vector<FunscriptAction> picked;
    picked.push_back(points[0]);
    int32_t lastDirection = 0;
    for (size_t i = 1, n = points.size(); i < n; i += 1) {
        const int32_t direction = (points[i].pos > points[i - 1].pos) - (points[i].pos < points[i - 1].pos);
        if (direction == 0) continue;
        if (lastDirection != 0 && direction != lastDirection && !(points[i - 1] == picked.back())) {
            picked.push_back(points[i - 1]);
        }
        lastDirection = direction;
    }
    const auto& last = points[points.size() - 1];
    if (!(last == picked.back())) picked.push_back(last);
    if (picked.size() < 2) return turns;

    const bool startsAtBottom = picked[0].pos <= picked[1].pos;
    float theta = startsAtBottom ? 0.f : (float)M_PI;
    for (const auto& action : picked) {
        turns.push_back(StrokeTurn{ action.atS, theta, (float)action.pos });
        theta += (float)M_PI;
    }
    return turns;
}

// -1 to 1 for a phase angle, 0 being a bottom of the stroke.
static float MultiAxisWave(int32_t pattern, float theta) noexcept
{
    switch (pattern) {
        // Out one way at the bottom, the other way at the top.
        case MultiAxisFollow: return std::cos(theta);
        // Once each way over two strokes: one way at a bottom, the other at the next.
        case MultiAxisAlternate: return std::cos(theta * 0.5f);
        // Furthest mid stroke, one way going down and the other coming up.
        case MultiAxisCircle: return std::sin(theta);
        // Twice per stroke, crossing over at the bottom and the top, which
        // against the stroke traces a figure eight.
        case MultiAxisFigureEight: return std::sin(theta * 2.f);
        default: return 0.f;
    }
}

static FunscriptArray GenerateMultiAxis(const std::vector<StrokeTurn>& turns, const MultiAxisAxisSettings& settings) noexcept
{
    FunscriptArray out;
    if (turns.size() < 2 || settings.pattern == MultiAxisOff) return out;

    float largestStroke = 0.f;
    for (size_t k = 0; k + 1 < turns.size(); k += 1) {
        largestStroke = std::max(largestStroke, std::abs(turns[k + 1].pos - turns[k].pos));
    }

    // Enough points between two turns for the wave to keep its shape once
    // the device draws straight lines between them.
    int32_t samples = 3;
    if (settings.pattern == MultiAxisFigureEight) samples = 6;
    else if (settings.pattern == MultiAxisFollow && settings.phase == 0 && settings.bottomFocus == 0) samples = 1;

    const float phase = (float)settings.phase * (float)M_PI / 180.f;
    const float direction = settings.reverse ? -1.f : 1.f;
    const float focus = Util::Clamp(settings.bottomFocus, 0, 100) / 100.f;

    auto valueAt = [&](float theta, float depth, float strokeSize) noexcept -> int32_t {
        // depth is 1 at a bottom and 0 at a top.
        const float focusWeight = 1.f + ((depth - 1.f) * focus);
        const float strokeWeight = settings.scaleWithStroke && largestStroke > 0.f ? strokeSize / largestStroke : 1.f;
        const float value = (float)settings.center
            + (direction * (float)settings.amount * MultiAxisWave(settings.pattern, theta + phase) * focusWeight * strokeWeight);
        return (int32_t)std::round(Util::Clamp(value, 0.f, 100.f));
    };

    FunscriptArray raw;
    int32_t lastMs = -1;
    auto push = [&](float atS, int32_t pos) noexcept {
        const int32_t ms = (int32_t)std::round(atS * 1000.f);
        if (ms <= lastMs) return;
        lastMs = ms;
        raw.emplace_back_unsorted(FunscriptAction(atS, pos));
    };

    for (size_t k = 0; k + 1 < turns.size(); k += 1) {
        const auto& a = turns[k];
        const auto& b = turns[k + 1];
        const float low = std::min(a.pos, b.pos);
        const float size = std::abs(b.pos - a.pos);
        for (int32_t j = 0; j < samples; j += 1) {
            const float t = (float)j / (float)samples;
            const float pos = a.pos + ((b.pos - a.pos) * t);
            const float depth = size > 0.f ? 1.f - ((pos - low) / size) : 1.f;
            push(a.atS + ((b.atS - a.atS) * t), valueAt(a.theta + ((b.theta - a.theta) * t), depth, size));
        }
    }
    {
        const auto& before = turns[turns.size() - 2];
        const auto& last = turns[turns.size() - 1];
        const float size = std::abs(last.pos - before.pos);
        const float depth = last.pos <= before.pos ? 1.f : 0.f;
        push(last.atS, valueAt(last.theta, depth, size));
    }

    // The middle of a run at one position adds nothing.
    for (size_t i = 0, n = raw.size(); i < n; i += 1) {
        if (i > 0 && i + 1 < n && raw[i - 1].pos == raw[i].pos && raw[i + 1].pos == raw[i].pos) continue;
        out.emplace_back_unsorted(raw[i]);
    }
    return out;
}

// The loaded script for a channel, added next to the first script as
// name.channel.funscript when there is none, as Project > Add > Shortcuts does.
static std::shared_ptr<Funscript> FindOrAddAxisScript(const char* channel) noexcept
{
    auto app = OpenFunscripter::ptr;
    for (const auto& script : app->LoadedFunscripts()) {
        if (MultiAxisChannel(*script) == channel) return script;
    }
    if (app->LoadedFunscripts().empty()) return nullptr;

    auto path = Util::PathFromString(app->LoadedProject->MakePathAbsolute(app->LoadedFunscripts()[0]->RelativePath()));
    path.replace_extension(Util::Format(".%s.funscript", channel));
    const size_t before = app->LoadedFunscripts().size();
    app->LoadedProject->AddFunscript(path.u8string());
    if (app->LoadedFunscripts().size() == before) return nullptr;
    return app->LoadedFunscripts().back();
}

MultiAxisGenerator::MultiAxisGenerator(uint32_t handle) noexcept
    : stateHandle(handle)
{
    eventUnsub = EV::MakeUnsubscibeFn(FunscriptSelectionChangedEvent::EventType, EV::Queue().appendListener(FunscriptSelectionChangedEvent::EventType,
        FunscriptSelectionChangedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &MultiAxisGenerator::SelectionChanged))));
    auto& state = SpecialFunctionState::State(stateHandle);
    for (int32_t i = 0; i < 3; i += 1) {
        if (MultiAxisSettingsFor(state, i).pattern != MultiAxisOff) { shownAxis = i; break; }
    }
}

MultiAxisGenerator::~MultiAxisGenerator() noexcept
{
    eventUnsub();
}

void MultiAxisGenerator::SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept
{
    // A preview keeps the points it was started from, so a new selection does
    // not disturb it. Otherwise it is time to stop saying what was applied.
    if (!previewing && ev->Script == OpenFunscripter::ptr->ActiveFunscript().get()) {
        appliedAxes.clear();
    }
}

void MultiAxisGenerator::Hidden() noexcept
{
    discardPreview();
}

void MultiAxisGenerator::discardPreview() noexcept
{
    auto app = OpenFunscripter::ptr;
    if (previewing && app->undoSystem->MatchUndoTop(StateType::GENERATE_ACTIONS)) {
        app->undoSystem->Undo();
    }
    previewing = false;
}

void MultiAxisGenerator::generate() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    // A copy: adding a script can grow the vector the active one lives in.
    std::shared_ptr<Funscript> source = app->ActiveFunscript();
    auto& state = SpecialFunctionState::State(stateHandle);
    const auto turns = FindStrokeTurns(previewSource);
    if (turns.size() < 2) return;

    // The preview already written is taken back first, so each change
    // replaces it, and axes turned off go back to what they had.
    discardPreview();

    std::array<std::shared_ptr<Funscript>, 3> targets;
    UndoContextScripts snapshot;
    previewAxes.clear();
    for (int32_t i = 0; i < 3; i += 1) {
        if (MultiAxisSettingsFor(state, i).pattern == MultiAxisOff) continue;
        auto target = FindOrAddAxisScript(MultiAxisLabels[i]);
        if (target == nullptr || target == source) continue;
        targets[i] = target;
        snapshot.emplace_back(target);
        if (!previewAxes.empty()) previewAxes += ", ";
        previewAxes += MultiAxisLabels[i];
    }
    if (snapshot.empty()) return;
    app->undoSystem->Snapshot(StateType::GENERATE_ACTIONS, std::move(snapshot));

    const float fromS = turns.front().atS;
    const float toS = turns.back().atS;
    for (int32_t i = 0; i < 3; i += 1) {
        if (targets[i] == nullptr) continue;
        auto actions = GenerateMultiAxis(turns, MultiAxisSettingsFor(state, i));
        targets[i]->RemoveActionsInInterval(fromS, toS);
        targets[i]->AddMultipleActions(actions);
    }
    previewing = true;
}

void MultiAxisGenerator::DrawUI() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    auto& state = SpecialFunctionState::State(stateHandle);
    bool changed = false;

    // Presets set all three axes at once: a starting point for a feel.
    {
        struct Preset { const char* name; const char* tip; };
        static constexpr Preset presets[4] = {
            { "Sway", "Roll leans left on one stroke and right on the next, most at the bottom." },
            { "Corkscrew", "Twist turns one way going down and back coming up." },
            { "Grind", "Pitch swings through at the bottom of each stroke, so the sleeve traces a figure eight." },
            { "Orbit", "Roll and pitch a quarter stroke apart, so the sleeve circles as it strokes." },
        };
        ImGui::TextDisabled("Presets");
        const auto& style = ImGui::GetStyle();
        const float width = (ImGui::GetContentRegionAvail().x - (style.ItemSpacing.x * 3.f)) / 4.f;
        for (int32_t i = 0; i < 4; i += 1) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::Button(presets[i].name, ImVec2(width, 0.f))) {
                MultiAxisAxisSettings off;
                state.twist = off;
                state.roll = off;
                state.pitch = off;
                switch (i) {
                    case 0: state.roll = { MultiAxisAlternate, 20, 50, 30, 0, true, false }; shownAxis = 1; break;
                    case 1: state.twist = { MultiAxisFollow, 30, 50, 0, 0, true, false }; shownAxis = 0; break;
                    case 2: state.pitch = { MultiAxisFigureEight, 25, 50, 70, 0, true, false }; shownAxis = 2; break;
                    case 3:
                        state.roll = { MultiAxisCircle, 15, 50, 0, 0, true, false };
                        state.pitch = { MultiAxisFollow, 15, 50, 0, 0, true, false };
                        shownAxis = 1;
                        break;
                }
                changed = true;
            }
            OFS::Tooltip(presets[i].tip);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Axis");
    {
        static constexpr const char* labels[3] = { "Twist", "Roll", "Pitch" };
        static constexpr const char* tips[3] = {
            "Turning around the length of the sleeve.",
            "Leaning left and right.",
            "Tilting forwards and back.",
        };
        const int32_t picked = OFS::SegmentedControl("##MultiAxisAxis", labels, tips, 3, shownAxis);
        if (picked >= 0) shownAxis = picked;
    }

    auto& axis = MultiAxisSettingsFor(state, shownAxis);
    ImGui::TextDisabled("Motion");
    {
        static constexpr const char* labels[MultiAxisPatternCount] = { "Off", "Follow", "Alternate", "Circle", "Figure 8" };
        static constexpr const char* tips[MultiAxisPatternCount] = {
            "Leaves this axis as it is.",
            "One way at the bottom of each stroke and the other way at the top.",
            "One way at the bottom of one stroke and the other way at the bottom of the next.",
            "Furthest mid stroke: one way going down and the other coming up. With another axis on Follow, the two make a circle.",
            "Swings twice per stroke, crossing over at the bottom and the top, so against the stroke it traces a figure eight. Raise bottom focus for a grind at the bottom.",
        };
        const int32_t picked = OFS::SegmentedControl("##MultiAxisPattern", labels, tips, MultiAxisPatternCount, axis.pattern);
        if (picked >= 0 && picked != axis.pattern) {
            axis.pattern = picked;
            changed = true;
        }
    }

    if (axis.pattern != MultiAxisOff) {
        changed |= OFS::StepperInt("Amount", "##MultiAxisAmount", &axis.amount, 5, 0, 50);
        OFS::Tooltip("How far it moves each way from the centre.");
        changed |= OFS::StepperInt("Centre", "##MultiAxisCentre", &axis.center, 5, 0, 100);
        switch (shownAxis) {
            case 0: OFS::Tooltip("Where twist rests. 50 turns as far each way."); break;
            case 1: OFS::Tooltip("Where roll rests. Keep it at 50 to lean as far left as right."); break;
            default: OFS::Tooltip("Where pitch rests. Move it off 50 to tilt the whole motion forwards or back."); break;
        }
        changed |= OFS::StepperInt("Bottom focus (%)", "##MultiAxisFocus", &axis.bottomFocus, 10, 0, 100);
        OFS::Tooltip("How much of the motion gathers at the bottom of the stroke. 0 moves as much at the top as at the bottom; 100 does not move at the top at all.");
        changed |= OFS::StepperInt("Shift (degrees)", "##MultiAxisShift", &axis.phase, 45, -180, 180);
        OFS::Tooltip("Moves the motion along the stroke. 180 is half a stroke, so a peak at the bottom moves to the top.");
        changed |= ImGui::Checkbox("Scale with stroke length", &axis.scaleWithStroke);
        OFS::Tooltip("Shorter strokes move less, in proportion to the longest stroke selected.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Reverse", &axis.reverse);
        OFS::Tooltip("Moves the other way.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    auto source = app->ActiveFunscript();
    const uint32_t selected = source->SelectionSize();
    std::string axesOn;
    for (int32_t i = 0; i < 3; i += 1) {
        if (MultiAxisSettingsFor(state, i).pattern == MultiAxisOff) continue;
        if (!axesOn.empty()) axesOn += ", ";
        axesOn += MultiAxisLabels[i];
    }
    const bool canPreview = selected >= 3 && !axesOn.empty();

    // Taken back by an undo, or kept by an edit made on top of it.
    if (previewing && !app->undoSystem->MatchUndoTop(StateType::GENERATE_ACTIONS)) {
        if (!app->undoSystem->MatchRedoTop(StateType::GENERATE_ACTIONS)) appliedAxes = previewAxes;
        previewing = false;
    }

    if (changed && previewing) {
        generate();
    }

    if (!previewing) {
        ImGui::BeginDisabled(!canPreview);
        if (ImGui::Button("Preview", ImVec2(-1.f, 0.f))) {
            previewSource = source->Selection();
            appliedAxes.clear();
            generate();
        }
        ImGui::EndDisabled();
        OFS::Tooltip("Write the motion into the axis scripts to see and feel it. Nothing is kept until you apply it.");
    }
    else {
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.f;
        ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::PinkFill));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
        if (ImGui::Button("Apply", ImVec2(half, 0.f))) {
            previewing = false;
            appliedAxes = previewAxes;
        }
        ImGui::PopStyleColor(3);
        OFS::Tooltip("Keep the preview in the scripts.");
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(half, 0.f))) {
            discardPreview();
        }
        OFS::Tooltip("Take the preview back out, leaving the scripts as they were.");
    }

    ImGui::PushTextWrapPos(0.f);
    if (previewing) {
        ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::V4(OFS_Sashimi::PinkPale));
        ImGui::TextUnformatted("Previewing, not applied yet.");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%s from %.2f s to %.2f s. Changes above update it; axes set to Off keep what they had. "
            "Closing this tool discards it.",
            previewAxes.c_str(), previewSource[0].atS, previewSource[previewSource.size() - 1].atS);
    }
    else if (selected < 3) {
        ImGui::TextDisabled("Select the stroke points to follow, at least 3. %u selected.", selected);
        if (!appliedAxes.empty()) ImGui::TextDisabled("Applied to %s.", appliedAxes.c_str());
    }
    else if (axesOn.empty()) {
        ImGui::TextDisabled("Every axis is off. Pick a motion for at least one.");
        if (!appliedAxes.empty()) ImGui::TextDisabled("Applied to %s.", appliedAxes.c_str());
    }
    else {
        const auto& selection = source->Selection();
        ImGui::TextDisabled("Preview writes %s from %.2f s to %.2f s, replacing what is there and adding a script "
            "for any axis the project does not have. Nothing is kept until you apply it.",
            axesOn.c_str(), selection[0].atS, selection[selection.size() - 1].atS);
        if (!appliedAxes.empty()) ImGui::TextDisabled("Applied to %s. Undo takes it back.", appliedAxes.c_str());
        if (MultiAxisChannel(*source) != "stroke") {
            ImGui::TextDisabled("The selected points are not on the stroke script.");
        }
    }
    ImGui::PopTextWrapPos();
}

// ============================== beat fill ==============================

namespace {
// A stretch to write into, with the tempo to write it at.
struct BeatRange
{
    float start = 0.f;
    float end = 0.f;
    float bpm = 0.f;
    // Absolute time of a downbeat. Only its remainder against the step matters,
    // so it can be a time far outside the range.
    float downbeat = 0.f;
    std::string name;
};
}

// The first grid time at or after start, in phase with the downbeat.
static float alignedStart(float start, float step, float downbeat) noexcept
{
    if (step <= 0.f) return start;
    const float steps = std::ceil((start - downbeat) / step);
    return downbeat + (steps * step);
}

static Chapter* chapterAtTime(std::vector<Chapter>& chapters, float time) noexcept
{
    for (auto& chapter : chapters) {
        if (time >= chapter.startTime && time <= chapter.endTime) return &chapter;
    }
    return nullptr;
}

// Collects what a fill will write into. A chapter carries the tempo detection
// stamped on it, which is what the grid follows; anything else follows the
// grid's own numbers, so a project with no chapters can still be filled.
static std::vector<BeatRange> collectRanges(int32_t scope, uint32_t tempoStateHandle, std::string& error) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& chapters = app->chapterMgr->State().chapters;
    const auto& tempo = TempoOverlayState::State(tempoStateHandle);
    std::vector<BeatRange> ranges;

    auto named = [](const Chapter& chapter) noexcept {
        return chapter.name.empty() ? std::string("a chapter") : chapter.name;
    };

    if (scope == BeatFillSelection) {
        const auto& selection = app->ActiveFunscript()->Selection();
        if (selection.size() < 2) {
            error = "Select the stretch to fill first: at least two points, marking its ends.";
            return ranges;
        }
        BeatRange range;
        range.start = selection.front().atS;
        range.end = selection.back().atS;
        range.name = "the selection";
        auto* chapter = chapterAtTime(chapters, range.start);
        if (chapter != nullptr && !chapter->isBreak && chapter->bpm > 0.f) {
            range.bpm = chapter->bpm;
            range.downbeat = chapter->measureOffsetSeconds;
        }
        else {
            range.bpm = tempo.bpm;
            range.downbeat = tempo.beatOffsetSeconds;
        }
        ranges.push_back(range);
    }
    else if (scope == BeatFillAllMusic) {
        for (auto& chapter : chapters) {
            if (chapter.isBreak || chapter.bpm <= 0.f) continue;
            ranges.push_back(BeatRange{ chapter.startTime, chapter.endTime, chapter.bpm,
                chapter.measureOffsetSeconds, named(chapter) });
        }
        if (ranges.empty()) {
            error = "No chapter has a tempo yet. Measure them in the Chapters panel first.";
        }
    }
    else {
        const float playhead = app->player->CurrentTime();
        auto* chapter = chapterAtTime(chapters, playhead);
        if (chapter == nullptr) {
            // No chapters at all is where every project starts, and the grid's
            // own tempo is then the only one there is.
            BeatRange range;
            range.start = 0.f;
            range.end = app->player->Duration();
            range.bpm = tempo.bpm;
            range.downbeat = tempo.beatOffsetSeconds;
            range.name = "the whole timeline";
            ranges.push_back(range);
        }
        else if (chapter->isBreak) {
            error = "The chapter under the playhead is marked as having no music in it.";
        }
        else if (chapter->bpm <= 0.f) {
            error = "The chapter under the playhead has no tempo yet. Measure it in the Chapters panel.";
        }
        else {
            ranges.push_back(BeatRange{ chapter->startTime, chapter->endTime, chapter->bpm,
                chapter->measureOffsetSeconds, named(*chapter) });
        }
    }

    for (const auto& range : ranges) {
        if (range.bpm <= 0.f || range.end <= range.start) {
            error = "There is no tempo to fill against. Measure a chapter, or set one on the tempo grid.";
            ranges.clear();
            break;
        }
    }
    return ranges;
}

BeatFill::BeatFill(uint32_t stateHandle) noexcept
    : stateHandle(stateHandle)
{
    tempoStateHandle = OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName);
}

BeatFill::~BeatFill() noexcept
{
}

void BeatFill::fill() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& state = SpecialFunctionState::State(stateHandle);
    lastResultIsError = false;

    std::string error;
    auto ranges = collectRanges(state.beatFillScope, tempoStateHandle, error);
    if (ranges.empty()) {
        lastResult = error;
        lastResultIsError = true;
        return;
    }

    const StrokePattern* pattern = nullptr;
    if (!state.beatFillPattern.empty()) {
        for (const auto& saved : state.patterns) {
            if (saved.name == state.beatFillPattern) { pattern = &saved; break; }
        }
        if (pattern == nullptr) {
            lastResult = "That pattern is no longer saved. Pick another.";
            lastResultIsError = true;
            return;
        }
    }

    const int32_t top = std::max(state.beatFillTop, state.beatFillBottom);
    const int32_t bottom = std::min(state.beatFillTop, state.beatFillBottom);

    // Everything is worked out before anything is written, so a fill that comes
    // to nothing leaves no undo step behind.
    std::vector<std::pair<const BeatRange*, FunscriptArray>> written;
    int32_t pointCount = 0;
    for (const auto& range : ranges) {
        const float beatSeconds = 60.f / range.bpm;
        FunscriptArray actions;

        if (pattern != nullptr) {
            const float stepSeconds = pattern->lengthBeats * beatSeconds;
            if (stepSeconds < 0.01f) continue;
            for (float at = alignedStart(range.start, stepSeconds, range.downbeat);
                 at < range.end; at += stepSeconds) {
                for (const auto& point : pattern->points) {
                    const float time = at + (point.beat * beatSeconds);
                    if (time < range.start || time > range.end) continue;
                    // The saved shape is stretched into the depth asked for
                    // here, so one pattern serves a gentle passage and a hard one.
                    const int32_t pos = bottom + (int32_t)std::lround(
                        (float)(top - bottom) * (Util::Clamp(point.pos, 0, 100) / 100.f));
                    actions.emplace(FunscriptAction(time, pos));
                }
            }
        }
        else {
            const uint32_t spacing = state.beatFillSpacing >= 0
                    && state.beatFillSpacing < (int32_t)TempoOverlay::beatMultiples.size()
                ? (uint32_t)state.beatFillSpacing : 2u;
            const float stepSeconds = beatSeconds * TempoOverlay::beatMultiples[spacing];
            if (stepSeconds < 0.01f) continue;
            bool atBottom = state.beatFillStartAtBottom;
            for (float at = alignedStart(range.start, stepSeconds, range.downbeat);
                 at <= range.end; at += stepSeconds) {
                actions.emplace(FunscriptAction(at, atBottom ? bottom : top));
                atBottom = !atBottom;
            }
        }

        if (actions.empty()) continue;
        pointCount += (int32_t)actions.size();
        written.emplace_back(&range, std::move(actions));
    }

    if (written.empty()) {
        lastResult = "Nothing to write: the range is shorter than one step of the grid.";
        lastResultIsError = true;
        return;
    }

    app->undoSystem->Snapshot(StateType::BEAT_FILL, app->ActiveFunscript());
    for (auto& entry : written) {
        if (state.beatFillReplace) {
            ctx().RemoveActionsInInterval(entry.first->start, entry.first->end);
        }
        ctx().AddMultipleActions(entry.second);
    }

    char message[160];
    if (written.size() == 1) {
        stbsp_snprintf(message, sizeof(message), "%d points across %s, at %.1f BPM.",
            pointCount, written.front().first->name.c_str(), written.front().first->bpm);
    }
    else {
        stbsp_snprintf(message, sizeof(message), "%d points across %d chapters.",
            pointCount, (int32_t)written.size());
    }
    lastResult = message;
}

void BeatFill::savePattern() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& state = SpecialFunctionState::State(stateHandle);
    const auto& selection = app->ActiveFunscript()->Selection();
    lastResultIsError = false;

    if (selection.size() < 2) {
        lastResult = "Select the points to save as a pattern first.";
        lastResultIsError = true;
        return;
    }
    if (patternName.empty()) {
        lastResult = "Give the pattern a name.";
        lastResultIsError = true;
        return;
    }

    std::string error;
    auto ranges = collectRanges(BeatFillSelection, tempoStateHandle, error);
    if (ranges.empty()) {
        lastResult = error;
        lastResultIsError = true;
        return;
    }
    const float beatSeconds = 60.f / ranges.front().bpm;

    StrokePattern saved;
    saved.name = patternName;
    const float first = selection.front().atS;
    for (const auto& action : selection) {
        saved.points.push_back(PatternPoint{ (action.atS - first) / beatSeconds, (int32_t)action.pos });
    }
    // Rounded up to a whole beat, so repeats land on the beat rather than
    // drifting by whatever the selection happened to end on.
    const float spanBeats = (selection.back().atS - first) / beatSeconds;
    saved.lengthBeats = std::max(1.f, std::ceil(spanBeats));

    for (auto& existing : state.patterns) {
        if (existing.name == saved.name) {
            existing = saved;
            state.beatFillPattern = saved.name;
            lastResult = "Replaced the pattern \"" + saved.name + "\".";
            savingPattern = false;
            patternName.clear();
            return;
        }
    }
    state.patterns.push_back(saved);
    state.beatFillPattern = saved.name;
    lastResult = "Saved \"" + saved.name + "\", " + std::to_string((int32_t)saved.lengthBeats) + " beats long.";
    savingPattern = false;
    patternName.clear();
}

void BeatFill::DrawUI() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto& state = SpecialFunctionState::State(stateHandle);

    {
        static constexpr const char* labels[BeatFillScopeCount] = { "Chapter", "Selection", "All music" };
        static constexpr const char* tips[BeatFillScopeCount] = {
            "The chapter under the playhead, at the tempo measured for it.",
            "Between the first and the last selected point.",
            "Every chapter with a tempo, skipping the ones marked as having no music.",
        };
        ImGui::TextDisabled("Fill");
        const int32_t picked = OFS::SegmentedControl("##BeatFillScope", labels, tips,
            BeatFillScopeCount, state.beatFillScope);
        if (picked >= 0) state.beatFillScope = picked;
    }

    // The saved patterns, with the built-in stroke first.
    {
        ImGui::TextDisabled("With");
        const char* current = state.beatFillPattern.empty() ? "Up and down" : state.beatFillPattern.c_str();
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##BeatFillPattern", current)) {
            if (ImGui::Selectable("Up and down", state.beatFillPattern.empty())) {
                state.beatFillPattern.clear();
            }
            for (const auto& saved : state.patterns) {
                if (ImGui::Selectable(saved.name.c_str(), saved.name == state.beatFillPattern)) {
                    state.beatFillPattern = saved.name;
                }
            }
            ImGui::EndCombo();
        }
        OFS::Tooltip("A plain stroke, or a passage saved from a selection and stamped along the beat.");
    }

    if (state.beatFillPattern.empty()) {
        ImGui::TextDisabled("A point every");
        ImGui::SetNextItemWidth(-1.f);
        {
            static constexpr const char* names[] = {
                "1/1", "1/2", "1/4", "1/8", "1/12", "1/16", "1/24", "1/32", "1/48", "1/64"
            };
            const int32_t count = (int32_t)TempoOverlay::beatMultiples.size();
            const int32_t current = state.beatFillSpacing >= 0 && state.beatFillSpacing < count
                ? state.beatFillSpacing : 2;
            if (ImGui::BeginCombo("##BeatFillSpacing", names[current], ImGuiComboFlags_HeightLarge)) {
                for (int32_t i = 0; i < count; i += 1) {
                    if (ImGui::Selectable(names[i], i == current)) state.beatFillSpacing = i;
                }
                ImGui::EndCombo();
            }
        }
        OFS::Tooltip("Note length between points. A point every quarter note is a stroke every two beats.");
        ImGui::Checkbox("Start at the bottom", &state.beatFillStartAtBottom);
    }

    OFS::StepperInt("Top", "##BeatFillTop", &state.beatFillTop, 5, 0, 100);
    OFS::StepperInt("Bottom", "##BeatFillBottom", &state.beatFillBottom, 5, 0, 100);
    ImGui::Checkbox("Replace what is there", &state.beatFillReplace);
    OFS::Tooltip("Clears the range before filling it. Without this the new points join what is already there.");

    ImGui::Spacing();
    if (ImGui::Button("Fill", ImVec2(-1.f, 0.f))) fill();

    // Saving a pattern is part of the same tool: a passage is selected, named,
    // and from then on it is in the list above.
    if (!savingPattern) {
        if (ImGui::Button("Save selection as a pattern", ImVec2(-1.f, 0.f))) {
            savingPattern = true;
            patternName.clear();
        }
        OFS::Tooltip("Keeps the selected points as a shape measured in beats, to stamp at any tempo.");
    }
    else {
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##PatternName", "Name for this pattern", &patternName);
        if (ImGui::Button("Save")) savePattern();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) savingPattern = false;
    }
    if (!state.beatFillPattern.empty() && ImGui::Button("Delete this pattern", ImVec2(-1.f, 0.f))) {
        auto& patterns = state.patterns;
        const std::string removed = state.beatFillPattern;
        patterns.erase(std::remove_if(patterns.begin(), patterns.end(),
            [&removed](const StrokePattern& saved) noexcept { return saved.name == removed; }),
            patterns.end());
        lastResult = "Deleted \"" + removed + "\".";
        lastResultIsError = false;
        state.beatFillPattern.clear();
    }

    if (!lastResult.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        if (lastResultIsError) ImGui::TextUnformatted(lastResult.c_str());
        else ImGui::TextDisabled("%s", lastResult.c_str());
        ImGui::PopTextWrapPos();
    }
}

// ============================ depth from loudness ============================

LoudnessDepth::LoudnessDepth(uint32_t stateHandle) noexcept
    : stateHandle(stateHandle)
{
}

LoudnessDepth::~LoudnessDepth() noexcept
{
}

void LoudnessDepth::apply() noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& state = SpecialFunctionState::State(stateHandle);
    lastResultIsError = false;

    const auto& selection = app->ActiveFunscript()->Selection();
    if (selection.size() < 3) {
        lastResult = "Select the points to scale first: at least three.";
        lastResultIsError = true;
        return;
    }

    // The same envelope the waveform and tempo detection read, so no audio is
    // decoded a second time.
    const auto& envelope = app->scriptTimeline.Wave.data.Samples();
    const float duration = app->player->Duration();
    if (envelope.empty() || duration <= 0.f) {
        lastResult = "No waveform yet. Make one from the timeline's right click menu.";
        lastResultIsError = true;
        return;
    }
    const float rate = (float)envelope.size() / duration;

    // A section is followed with a window of a couple of seconds; single beats
    // with one about as long as the gap between the points themselves.
    float window = 2.f;
    if (!state.loudnessSections) {
        const float span = selection.back().atS - selection.front().atS;
        window = Util::Clamp(span / (float)selection.size(), 0.05f, 1.f);
    }

    std::vector<float> levels;
    levels.reserve(selection.size());
    for (const auto& action : selection) {
        const int32_t from = std::max(0, (int32_t)((action.atS - (window * 0.5f)) * rate));
        const int32_t to = std::min((int32_t)envelope.size() - 1, (int32_t)((action.atS + (window * 0.5f)) * rate));
        float sum = 0.f;
        int32_t count = 0;
        for (int32_t i = from; i <= to; i += 1) {
            sum += std::abs(envelope[i]);
            count += 1;
        }
        levels.push_back(count > 0 ? sum / (float)count : 0.f);
    }

    // Scaled against the quietest and loudest of the selection rather than
    // against absolute loudness, so a quiet recording is not scripted shallow
    // throughout. Percentiles rather than the extremes, so one loud crash does
    // not flatten everything else.
    std::vector<float> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    const float quiet = sorted[(size_t)(sorted.size() * 0.05f)];
    const float loud = sorted[(size_t)std::min(sorted.size() - 1, (size_t)(sorted.size() * 0.95f))];
    if (loud - quiet < 0.0001f) {
        lastResult = "The audio is equally loud across the selection, so there is nothing to follow.";
        lastResultIsError = true;
        return;
    }

    int32_t anchorPos = 50;
    if (state.loudnessAnchor == 1 || state.loudnessAnchor == 2) {
        int32_t lowest = 100;
        int32_t highest = 0;
        for (const auto& action : selection) {
            lowest = std::min(lowest, (int32_t)action.pos);
            highest = std::max(highest, (int32_t)action.pos);
        }
        anchorPos = state.loudnessAnchor == 1 ? lowest : highest;
    }
    else {
        int32_t lowest = 100;
        int32_t highest = 0;
        for (const auto& action : selection) {
            lowest = std::min(lowest, (int32_t)action.pos);
            highest = std::max(highest, (int32_t)action.pos);
        }
        anchorPos = (lowest + highest) / 2;
    }

    const float quietest = Util::Clamp(state.loudnessQuietDepth, 0, 100) / 100.f;
    FunscriptArray scaled;
    for (size_t i = 0; i < selection.size(); i += 1) {
        const float level = Util::Clamp((levels[i] - quiet) / (loud - quiet), 0.f, 1.f);
        const float depth = quietest + ((1.f - quietest) * level);
        const float moved = (float)anchorPos + (((float)selection[i].pos - (float)anchorPos) * depth);
        scaled.emplace(FunscriptAction(selection[i].atS,
            Util::Clamp((int32_t)std::lround(moved), 0, 100)));
    }

    app->undoSystem->Snapshot(StateType::LOUDNESS_DEPTH, app->ActiveFunscript());
    ctx().RemoveSelectedActions();
    ctx().AddMultipleActions(scaled);
    ctx().SetSelection(scaled);

    char message[160];
    stbsp_snprintf(message, sizeof(message), "Scaled %d points, quietest to %d%% of their depth.",
        (int32_t)scaled.size(), Util::Clamp(state.loudnessQuietDepth, 0, 100));
    lastResult = message;
}

void LoudnessDepth::DrawUI() noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    auto& state = SpecialFunctionState::State(stateHandle);

    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Scales the selected strokes by how loud the audio is under them. "
                        "Quiet passages end up shallower, loud ones keep their full depth.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    OFS::StepperInt("Quietest keeps (%)", "##LoudnessQuiet", &state.loudnessQuietDepth, 5, 0, 100);
    OFS::Tooltip("How much of its depth the quietest passage of the selection keeps. "
                 "The loudest always keeps all of it.");

    {
        static constexpr const char* labels[3] = { "Middle", "Bottom", "Top" };
        static constexpr const char* tips[3] = {
            "Strokes shrink towards the middle of the selection.",
            "Bottoms stay put and tops come down.",
            "Tops stay put and bottoms come up.",
        };
        ImGui::TextDisabled("Shrink towards");
        const int32_t picked = OFS::SegmentedControl("##LoudnessAnchor", labels, tips, 3, state.loudnessAnchor);
        if (picked >= 0) state.loudnessAnchor = picked;
    }

    {
        static constexpr const char* labels[2] = { "Sections", "Beats" };
        static constexpr const char* tips[2] = {
            "Follows the shape of the track over a couple of seconds.",
            "Follows single hits, so one stroke can differ from the next.",
        };
        ImGui::TextDisabled("Follow");
        const int32_t picked = OFS::SegmentedControl("##LoudnessFollow", labels, tips, 2,
            state.loudnessSections ? 0 : 1);
        if (picked >= 0) state.loudnessSections = (picked == 0);
    }

    ImGui::Spacing();
    const bool haveWaveform = app->scriptTimeline.Wave.data.SampleCount() > 0;
    ImGui::BeginDisabled(!haveWaveform);
    if (ImGui::Button("Apply", ImVec2(-1.f, 0.f))) apply();
    ImGui::EndDisabled();
    if (!haveWaveform) {
        if (ImGui::Button("Make waveform", ImVec2(-1.f, 0.f))) {
            app->scriptTimeline.RequestWaveform();
        }
        OFS::Tooltip("Reads the audio once, which is what the loudness is taken from.");
    }

    if (!lastResult.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        if (lastResultIsError) ImGui::TextUnformatted(lastResult.c_str());
        else ImGui::TextDisabled("%s", lastResult.c_str());
        ImGui::PopTextWrapPos();
    }
}
