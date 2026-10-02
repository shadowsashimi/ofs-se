#include "OFS_ScriptCheck.h"

#include "OpenFunscripter.h"
#include "OFS_ImGui.h"
#include "OFS_Profiling.h"
#include "state/ScriptCheckState.h"

#include "imgui.h"
#include "stb_sprintf.h"

#include <algorithm>
#include <cmath>

namespace OFS_Check
{

static float speedBetween(FunscriptAction from, FunscriptAction to) noexcept
{
    const float seconds = to.atS - from.atS;
    if (seconds <= 0.f) return 0.f;
    return std::abs((float)to.pos - (float)from.pos) / seconds;
}

const char* KindName(ProblemKind kind) noexcept
{
    switch (kind) {
        case ProblemKind::TooFast: return "Too fast";
        case ProblemKind::TooClose: return "Too close";
        case ProblemKind::LongGap: return "Long gap";
        case ProblemKind::PastTheEnd: return "Past the end";
    }
    return "";
}

std::vector<Problem> Run(const FunscriptArray& actions, const Limits& limits, float duration) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<Problem> problems;
    if (actions.size() < 2) return problems;

    for (int32_t i = 0; i < (int32_t)actions.size() - 1; i += 1) {
        const auto from = actions[i];
        const auto to = actions[i + 1];
        const float gap = to.atS - from.atS;

        if (gap < limits.minGapSeconds) {
            problems.push_back(Problem{ ProblemKind::TooClose, from.atS, from, to, gap });
            // One complaint per pair: a pair this close is usually also over
            // the speed limit, and saying both of a single millisecond apart
            // is noise.
            continue;
        }
        const float speed = speedBetween(from, to);
        if (speed > limits.maxSpeed) {
            problems.push_back(Problem{ ProblemKind::TooFast, from.atS, from, to, speed });
        }
        else if (gap > limits.longGapSeconds) {
            problems.push_back(Problem{ ProblemKind::LongGap, from.atS, from, to, gap });
        }
    }

    if (duration > 0.f) {
        for (const auto& action : actions) {
            if (action.atS > duration + 0.001f) {
                problems.push_back(Problem{ ProblemKind::PastTheEnd, action.atS, action, action,
                    action.atS - duration });
            }
        }
    }

    std::sort(problems.begin(), problems.end(), [](const Problem& a, const Problem& b) noexcept {
        return a.atTime < b.atTime;
    });
    return problems;
}

FunscriptArray LimitSpeed(const FunscriptArray& actions, const Limits& limits) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    FunscriptArray limited;
    if (actions.empty()) return limited;

    limited.emplace_back_unsorted(actions[0]);
    for (int32_t i = 1; i < (int32_t)actions.size(); i += 1) {
        auto action = actions[i];
        const auto previous = limited.back();
        const float seconds = action.atS - previous.atS;
        if (seconds > 0.f) {
            const float reach = limits.maxSpeed * seconds;
            const float wanted = (float)action.pos - (float)previous.pos;
            if (std::abs(wanted) > reach) {
                // Pulled back towards the point before it, which shortens the
                // stroke and leaves its timing alone. Moving it in time
                // instead would drag it off the beat it was written on.
                const float clamped = (float)previous.pos + (wanted > 0.f ? reach : -reach);
                // Rounded towards the point before it rather than to the
                // nearest whole position: rounding away lands a unit past the
                // reach, which is over the limit again by a hair and comes
                // back on the next check.
                const float whole = wanted > 0.f ? std::floor(clamped) : std::ceil(clamped);
                action.pos = (int16_t)Util::Clamp((int32_t)whole, 0, 100);
            }
        }
        limited.emplace_back_unsorted(action);
    }
    return limited;
}

FunscriptArray ThinOut(const FunscriptArray& actions, const Limits& limits) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    FunscriptArray thinned;
    if (actions.empty()) return thinned;

    thinned.emplace_back_unsorted(actions[0]);
    for (int32_t i = 1; i < (int32_t)actions.size(); i += 1) {
        const auto action = actions[i];
        const auto previous = thinned.back();
        if (action.atS - previous.atS >= limits.minGapSeconds) {
            thinned.emplace_back_unsorted(action);
            continue;
        }
        // Too close to the one kept. The pair becomes whichever of the two is
        // further from the point before them, so a peak survives and the
        // duplicate beside it goes.
        if (thinned.size() >= 2) {
            const auto before = thinned[thinned.size() - 2];
            if (std::abs((float)action.pos - (float)before.pos)
                > std::abs((float)previous.pos - (float)before.pos)) {
                thinned.pop_back();
                thinned.emplace_back_unsorted(action);
            }
        }
        else if (std::abs((float)action.pos - 50.f) > std::abs((float)previous.pos - 50.f)) {
            thinned.pop_back();
            thinned.emplace_back_unsorted(action);
        }
    }
    return thinned;
}

}

OFS_ScriptCheck::OFS_ScriptCheck() noexcept
{
    stateHandle = OFS_AppState<ScriptCheckState>::Register(ScriptCheckState::StateName);
}

OFS_Check::Limits OFS_ScriptCheck::CurrentLimits() const noexcept
{
    const auto& state = ScriptCheckState::State(stateHandle);
    OFS_Check::Limits limits;
    limits.maxSpeed = (float)state.maxSpeed;
    limits.minGapSeconds = (float)state.minGapMs / 1000.f;
    limits.longGapSeconds = (float)state.longGapSeconds;
    return limits;
}

void OFS_ScriptCheck::run() noexcept
{
    auto app = OpenFunscripter::ptr;
    if (!app->LoadedProject->IsValid()) return;
    auto script = app->ActiveFunscript();
    problems = OFS_Check::Run(script->Actions(), CurrentLimits(), app->player->Duration());
    checkedScript = script->Title();
    checkedOnce = true;
}

void OFS_ScriptCheck::ShowWindow(bool* open) noexcept
{
    if (open != nullptr && !*open) return;
    OFS_PROFILE(__FUNCTION__);
    auto app = OpenFunscripter::ptr;
    auto& state = ScriptCheckState::State(stateHandle);

    {
        const auto* viewport = ImGui::GetMainViewport();
        const float em = ImGui::GetFontSize();
        // Tall enough for the limits, the counts and a few rows of the list:
        // shorter, the list opened with its header alone showing.
        ImGui::SetNextWindowSize(ImVec2(em * 30.f, em * 34.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    }
    ImGui::Begin("Script check###SCRIPT_CHECK", open, ImGuiWindowFlags_None);

    // The numbers are what a device can follow, which varies by device, so
    // they are named by how hard they are rather than by a make and model.
    {
        static constexpr const char* labels[3] = { "Gentle", "Typical", "Fast" };
        static constexpr const char* tips[3] = {
            "300 units a second, 100ms apart. For a device that takes its time.",
            "400 units a second, 50ms apart. A reasonable middle.",
            "600 units a second, 20ms apart. For a quick one.",
        };
        ImGui::TextDisabled("Limits");
        const int32_t picked = OFS::SegmentedControl("##CheckPreset", labels, tips, 3, state.preset);
        if (picked >= 0) {
            state.preset = picked;
            const int32_t speeds[3] = { 300, 400, 600 };
            const int32_t gaps[3] = { 100, 50, 20 };
            state.maxSpeed = speeds[picked];
            state.minGapMs = gaps[picked];
            run();
        }
        OFS::Tooltip("Starting points, not a promise about any particular device. "
                     "Check what yours does and set the numbers to match.");
    }

    bool changed = false;
    changed |= OFS::StepperInt("Fastest stroke (units/s)", "##CheckSpeed", &state.maxSpeed, 50, 50, 2000);
    changed |= OFS::StepperInt("Closest points (ms)", "##CheckGap", &state.minGapMs, 10, 1, 500);
    changed |= OFS::StepperInt("Longest gap (s)", "##CheckLongGap", &state.longGapSeconds, 1, 1, 120);
    if (changed) run();

    ImGui::Spacing();
    const bool haveScript = app->LoadedProject->IsValid();
    ImGui::BeginDisabled(!haveScript);
    if (ImGui::Button("Check", ImVec2(-1.f, 0.f))) run();
    ImGui::EndDisabled();

    if (!checkedOnce) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Lists the strokes a device cannot follow: too fast to keep up with, "
                            "too close together to tell apart, long stretches with nothing in "
                            "them, and points past the end of the media.");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }

    // Counted by kind, so the shape of the problem is visible before reading
    // a list of a thousand rows.
    int32_t counts[4] = { 0, 0, 0, 0 };
    for (const auto& problem : problems) counts[(int32_t)problem.kind] += 1;

    ImGui::Spacing();
    if (problems.empty()) {
        ImGui::TextDisabled("Nothing to report in \"%s\".", checkedScript.c_str());
        ImGui::End();
        return;
    }

    ImGui::Text("%d to look at in \"%s\"", (int32_t)problems.size(), checkedScript.c_str());
    ImGui::TextDisabled("%d too fast, %d too close, %d long gaps, %d past the end",
        counts[0], counts[1], counts[2], counts[3]);

    ImGui::Spacing();
    ImGui::BeginDisabled(counts[0] == 0);
    if (ImGui::Button("Slow the fast ones down")) {
        auto script = app->ActiveFunscript();
        app->undoSystem->Snapshot(StateType::LIMIT_SPEED, script);
        auto limited = OFS_Check::LimitSpeed(script->Actions(), CurrentLimits());
        script->SetActions(limited);
        run();
    }
    ImGui::EndDisabled();
    OFS::Tooltip("Pulls each point that arrives too soon back towards the one before it, "
                 "which shortens the stroke and leaves its timing alone.");
    ImGui::SameLine();
    ImGui::BeginDisabled(counts[1] == 0);
    if (ImGui::Button("Thin out the close ones")) {
        auto script = app->ActiveFunscript();
        app->undoSystem->Snapshot(StateType::THIN_OUT, script);
        auto thinned = OFS_Check::ThinOut(script->Actions(), CurrentLimits());
        script->SetActions(thinned);
        run();
    }
    ImGui::EndDisabled();
    OFS::Tooltip("Of each pair too close together, keeps the one that leaves the shape closer "
                 "to what it was.");

    ImGui::Spacing();
    if (ImGui::BeginTable("##Problems", 3,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthFixed, em * 5.f);
        ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthFixed, em * 7.f);
        ImGui::TableSetupColumn("How much", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Long lists are common on a first check, so only the rows on screen
        // are built.
        ImGuiListClipper clipper;
        clipper.Begin((int32_t)problems.size());
        while (clipper.Step()) {
            for (int32_t i = clipper.DisplayStart; i < clipper.DisplayEnd; i += 1) {
                const auto& problem = problems[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);

                char timeLabel[24];
                Util::FormatTime(timeLabel, sizeof(timeLabel), problem.atTime, true);
                if (ImGui::Selectable(timeLabel, false, ImGuiSelectableFlags_SpanAllColumns)) {
                    app->player->SetPositionExact(problem.atTime);
                    FunscriptArray pair;
                    pair.emplace(problem.from);
                    if (problem.to != problem.from) pair.emplace(problem.to);
                    app->ActiveFunscript()->SetSelection(pair);
                }
                OFS::Tooltip("Sends the playhead here and selects the points.");

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(OFS_Check::KindName(problem.kind));
                ImGui::TableNextColumn();
                switch (problem.kind) {
                    case OFS_Check::ProblemKind::TooFast:
                        ImGui::Text("%.0f units/s", problem.value);
                        break;
                    case OFS_Check::ProblemKind::TooClose:
                        ImGui::Text("%.0f ms apart", problem.value * 1000.f);
                        break;
                    case OFS_Check::ProblemKind::LongGap:
                        ImGui::Text("%.1f s of nothing", problem.value);
                        break;
                    case OFS_Check::ProblemKind::PastTheEnd:
                        ImGui::Text("%.1f s after the end", problem.value);
                        break;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::End();
}
