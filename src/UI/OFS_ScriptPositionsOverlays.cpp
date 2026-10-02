#include "OFS_ScriptPositionsOverlays.h"
#include "OpenFunscripter.h"

#include "OFS_ImGui.h"
#include "state/ProjectState.h"
#include "state/states/ChapterState.h"

void FrameOverlay::DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept
{
    auto app = OpenFunscripter::ptr;
    float fps = enableFpsOverride ? fpsOverride : app->player->Fps();
    float frameTime = enableFpsOverride ? (1.f / fpsOverride) : app->scripting->LogicalFrameTime();
    float visibleFrames = ctx.visibleTime / frameTime;
    constexpr float maxVisibleFrames = 400.f;
   
    if (visibleFrames <= (maxVisibleFrames * 0.75f)) {
        //render frame dividers
        float offset = -std::fmod(ctx.offsetTime, frameTime);
        const int lineCount = visibleFrames + 2;
        int alpha = 255 * (1.f - (visibleFrames / maxVisibleFrames));
        for (int i = 0; i < lineCount; i++) {
            ctx.drawList->AddLine(
                ctx.canvasPos + ImVec2(((offset + (i * frameTime)) / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
                ctx.canvasPos + ImVec2(((offset + (i * frameTime)) / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
                IM_COL32(80, 80, 80, alpha),
                1.f
            );
        }
    }

    // time dividers
    constexpr float maxVisibleTimeDividers = 150.f;
    const float timeIntervalMs = std::round(fps * 0.1f) * frameTime;
    const float visibleTimeIntervals = ctx.visibleTime / timeIntervalMs;
    if (visibleTimeIntervals <= (maxVisibleTimeDividers * 0.8f)) {
        float offset = -std::fmod(ctx.offsetTime, timeIntervalMs);
        const int lineCount = visibleTimeIntervals + 2;
        int alpha = 255 * (1.f - (visibleTimeIntervals / maxVisibleTimeDividers));
        for (int i = 0; i < lineCount; i++) {
            ctx.drawList->AddLine(
                ctx.canvasPos + ImVec2(((offset + (i * timeIntervalMs)) / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
                ctx.canvasPos + ImVec2(((offset + (i * timeIntervalMs)) / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
                IM_COL32(80, 80, 80, alpha),
                3.f
            );
        }
    }
    BaseOverlay::DrawHeightLines(ctx);
    timeline->DrawAudioWaveform(ctx);
    BaseOverlay::DrawActionLines(ctx);
    BaseOverlay::DrawActionPoints(ctx);
    BaseOverlay::DrawSecondsLabel(ctx);
 
    // out of sync line
    auto& state = BaseOverlay::State();
    if (state.SyncLineEnable) {
        float realFrameTime = app->player->CurrentPlayerTime() - ctx.offsetTime;
        ctx.drawList->AddLine(
            ctx.canvasPos + ImVec2((realFrameTime / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
            ctx.canvasPos + ImVec2((realFrameTime / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
            IM_COL32(255, 0, 0, 255),
            1.f
        );
    }
}

void FrameOverlay::nextFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    if(enableFpsOverride) {
        app->player->SeekRelative(1.f / fpsOverride);
    }
    else {
        app->player->NextFrame();
    }    
}

void FrameOverlay::previousFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    if(enableFpsOverride) {
        app->player->SeekRelative(-(1.f / fpsOverride));
    }
    else {
        app->player->PreviousFrame();
    }
}

float FrameOverlay::steppingIntervalBackward(float realFrameTime, float fromTime) noexcept
{
    return -logicalFrameTime(realFrameTime);
}

float FrameOverlay::steppingIntervalForward(float realFrameTime, float fromTime) noexcept
{
    return logicalFrameTime(realFrameTime);
}

float FrameOverlay::logicalFrameTime(float realFrameTime) noexcept
{
    return enableFpsOverride ? (1.f / fpsOverride) : realFrameTime;
}

float FrameOverlay::SnapTime(float time) noexcept
{
    // Frames are counted from zero, so there is no phase to carry here the way
    // the tempo grid has to.
    const float frameTime = logicalFrameTime(OpenFunscripter::ptr->scripting->LogicalFrameTime());
    if(frameTime <= 0.f) return time;
    return std::round(time / frameTime) * frameTime;
}

void FrameOverlay::DrawSettings() noexcept
{
    if(ImGui::Checkbox(TR_ID("FPS_OVERRIDE_ENABLE", Tr::FPS_OVERRIDE), &enableFpsOverride))
    {
        fpsOverride = OpenFunscripter::ptr->player->Fps();
    }
    if(enableFpsOverride) {
        if(OFS::StepperFloat("Frame rate (fps)", "##FpsOverride", &fpsOverride, 1.f, 1.f, 150.f, "%.3f"))
        {
            fpsOverride = Util::Clamp(fpsOverride, 1.f, 150.f);
            // snap to new framerate
            auto app = OpenFunscripter::ptr;
            float newPosition = std::round(app->player->CurrentTime() / (1.0 / (double)fpsOverride))
                * (1.0 / (double)fpsOverride);
            app->player->SetPositionExact(newPosition, true);
        }
    }
}

TempoOverlay::TempoOverlay(ScriptTimeline* timeline) noexcept
    : BaseOverlay(timeline)
{
    stateHandle = OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName);
    chapterStateHandle = OFS_ProjectState<ChapterState>::Register(ChapterState::StateName);
}

// Chapters carry the tempo they were measured at, so following the playhead is
// just a lookup. Run every frame rather than on a chapter change event: seeking
// and scrubbing both move the playhead without any state change to listen for,
// and a linear scan over a handful of chapters costs nothing next to the draw.
Chapter* TempoOverlay::chapterUnderPlayhead() noexcept
{
    const float currentTime = OpenFunscripter::ptr->player->CurrentTime();
    auto& chapters = ChapterState::State(chapterStateHandle).chapters;
    for(auto& chapter : chapters) {
        // Half open, the same rule the overlap test uses: an instant shared by
        // two chapters belongs to the later one.
        if(currentTime < chapter.startTime || currentTime >= chapter.endTime) continue;
        return &chapter;
    }
    return nullptr;
}

Chapter* TempoOverlay::tempoDriver(const TempoOverlayState& tempo) noexcept
{
    if(!tempo.autoTempo) return nullptr;
    auto* chapter = chapterUnderPlayhead();
    // A break, a chapter with no tempo recorded, and the gaps between chapters
    // all leave the grid where it is rather than resetting it, so it holds the
    // last tempo it was given instead of flickering through the parts with no
    // music to follow.
    if(chapter == nullptr || chapter->isBreak || chapter->bpm <= 0.f) return nullptr;
    return chapter;
}

void TempoOverlay::followChapterTempo(TempoOverlayState& tempo) noexcept
{
    if(auto* chapter = tempoDriver(tempo)) {
        tempo.SetFromTempo(chapter->bpm, chapter->measureOffsetSeconds);
    }
}

void TempoOverlay::writePhaseToChapter(const TempoOverlayState& tempo) noexcept
{
    if(auto* chapter = tempoDriver(tempo)) {
        chapter->measureOffsetSeconds = tempo.beatOffsetSeconds;
    }
}

void TempoOverlay::DrawSettings() noexcept
{
    BaseOverlay::DrawSettings();
    auto& tempo = TempoOverlayState::State(stateHandle);

    ImGui::Checkbox(TR(TEMPO_AUTO), &tempo.autoTempo);
    OFS::Tooltip("Follows the chapter under the playhead, using the tempo recorded on it "
                 "by detection. Without this the grid holds one tempo for the whole media "
                 "and has to be reapplied at every track.");

    // The BPM is an output only while a chapter is actually supplying it.
    // Greying it out on the strength of the checkbox alone left a project with
    // no chapters yet -- or a playhead sitting in a gap, or in a break -- with
    // no way to set a tempo at all, which is the state every project starts in.
    auto* driver = tempoDriver(tempo);

    // Every number here is typed, with steps either side, and named on a line
    // above it, so none reads as an unnamed number.
    // Editable while a chapter drives the grid too: a tempo typed then is set on
    // that chapter by hand, where it is kept, rather than refused.
    if(OFS::StepperFloat("Tempo (BPM)", "##TempoBpm", &tempo.bpm, 1.f, 1.f, 1000.f, "%.1f") && driver != nullptr) {
        driver->SetTempoByHand(tempo.bpm);
        EV::Enqueue<ChapterStateChanged>();
    }

    if(driver != nullptr) {
        // Says where the number came from, which is the thing a disabled field
        // otherwise leaves the user to guess at.
        ImGui::TextDisabled(driver->bpmManual ? "following \"%s\", set by hand" : "following \"%s\"",
            driver->name.c_str());
        OFS::Tooltip("The BPM is this chapter's. Typing one above sets it on the chapter by hand, "
                     "which measuring all chapters leaves alone; or count it half or double "
                     "time below.");
    }
    else if(tempo.autoTempo) {
        // Says why nothing is being followed, with the fix one click away.
        // Detection used to be reachable only from the Chapters window, so
        // ticking this box with no measured chapters did nothing anyone could
        // see, and nothing here said where the tempo was meant to come from.
        auto app = OpenFunscripter::ptr;
        auto& chapterMgr = *app->chapterMgr;
        const auto& chapters = ChapterState::State(chapterStateHandle).chapters;
        auto* here = chapterUnderPlayhead();
        const float fullWidth = ImGui::GetContentRegionAvail().x;

        ImGui::PushTextWrapPos(0.f);
        if(chapterMgr.Busy()) {
            ImGui::TextDisabled("Reading the audio...");
        }
        else if(chapters.empty()) {
            ImGui::TextDisabled("No chapters yet, so there is no tempo to follow.");
            if(ImGui::Button("Detect tempo and chapters", ImVec2(fullWidth, 0.f))) {
                chapterMgr.DetectTempo();
                app->ShowChapters();
            }
            OFS::Tooltip("Scans the audio, lays a chapter over each track and measures its tempo. "
                         "The grid then follows whichever chapter the playhead is in.");
        }
        else if(here == nullptr) {
            ImGui::TextDisabled("The playhead is between chapters, so the grid keeps the last tempo it had.");
        }
        else if(here->isBreak) {
            ImGui::TextDisabled("\"%s\" is marked as having no music, so the grid keeps the last tempo it had.",
                here->name.c_str());
        }
        else {
            ImGui::TextDisabled("\"%s\" has no tempo yet.", here->name.c_str());
            if(ImGui::Button("Measure this chapter", ImVec2(fullWidth, 0.f))) {
                if(!chapterMgr.MeasureChapter(*here)) app->ShowChapters();
            }
            OFS::Tooltip("Measures this chapter's tempo from the audio. The Chapters window can measure all of them at once.");
        }
        ImGui::PopTextWrapPos();
    }
    if(tempo.autoTempo) {
        if(ImGui::Button("Open the Chapters window", ImVec2(ImGui::GetContentRegionAvail().x, 0.f))) {
            OpenFunscripter::ptr->ShowChapters();
        }
        OFS::Tooltip("Opens the Chapters window, where the tempo of each chapter is detected, checked and corrected.");
    }

    // Half or double time, for a tempo read off the wrong pulse: music is often
    // felt in a beat twice as fast as the one its kick marks, or half as fast.
    // While a chapter drives the grid it is the chapter's tempo that changes,
    // so the correction is kept there and survives measuring it again.
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - spacing) / 2.f;
        auto scale = [&](float factor) noexcept {
            if(driver != nullptr) {
                driver->ScaleTempo(factor);
                tempo.SetFromTempo(driver->bpm, driver->measureOffsetSeconds);
                EV::Enqueue<ChapterStateChanged>();
            }
            else {
                tempo.SetFromTempo(Util::Clamp(tempo.bpm * factor, 1.f, 1000.f), tempo.beatOffsetSeconds);
            }
        };
        if(ImGui::Button("Half time /2", ImVec2(buttonWidth, 0.f))) scale(0.5f);
        OFS::Tooltip(driver != nullptr
            ? "Halves this chapter's tempo, for one read off a pulse twice as fast as the beat. "
              "Kept when the chapter is measured again."
            : "Halves the tempo.");
        ImGui::SameLine();
        if(ImGui::Button("Double time x2", ImVec2(buttonWidth, 0.f))) scale(2.f);
        OFS::Tooltip(driver != nullptr
            ? "Doubles this chapter's tempo, for one read off a pulse half as fast as the beat. "
              "Kept when the chapter is measured again."
            : "Doubles the tempo.");
    }

    // Detection picks the downbeat from onset energy alone, which cannot tell
    // the four beats of a bar apart when they are equally loud -- four on the
    // floor being the obvious case. Correcting it by ear is one click.
    //
    // All three of these say where the bar line goes, at three granularities,
    // so they sit together on one row instead of being spread down the panel
    // with the grid controls in between.
    // A heading over a row of three equal buttons. As small buttons on the
    // heading's own line the third ran out of the panel and was cut off.
    ImGui::TextDisabled("%s", TR(TEMPO_DOWNBEAT));
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float buttonWidth = (ImGui::GetContentRegionAvail().x - (spacing * 2.f)) / 3.f;
        if(ImGui::Button("-1 beat", ImVec2(buttonWidth, 0.f))) { tempo.NudgeDownbeat(-1); writePhaseToChapter(tempo); }
        OFS::Tooltip("Rotates the grid back by one beat, for when the bar lines are landing "
                     "on the wrong beat of the bar.");
        ImGui::SameLine();
        if(ImGui::Button("+1 beat", ImVec2(buttonWidth, 0.f))) { tempo.NudgeDownbeat(1); writePhaseToChapter(tempo); }
        OFS::Tooltip("Rotates the grid on by one beat, for when the bar lines are landing "
                     "on the wrong beat of the bar.");
        ImGui::SameLine();
        if(ImGui::Button("At playhead", ImVec2(buttonWidth, 0.f))) {
            tempo.SetDownbeatAt(OpenFunscripter::ptr->player->CurrentTime());
            writePhaseToChapter(tempo);
        }
        OFS::Tooltip("Puts a bar line exactly on the playhead. Park it on a beat you can "
                     "hear and the whole grid lines up from there, which is quicker than "
                     "arguing with the offset when detection has the wrong beat.");
    }

    // Stays editable while a chapter is driving, unlike the BPM, because the
    // other two phase controls do too and all three write the correction back
    // onto the chapter. Phase is the part that gets fixed by ear.
    if(OFS::StepperFloat("Offset (s)", "##TempoOffset", &tempo.beatOffsetSeconds, 0.01f, -10.f, 10.f, "%.3f")) {
        writePhaseToChapter(tempo);
    }
    OFS::Tooltip("Fine adjustment of the same bar line, for when it is out by less than "
                 "a beat.");

    ImGui::TextDisabled("Note spacing");
    ImGui::SetNextItemWidth(-1.f);
    DrawNoteDivisionSelector("##TempoGrid", true);
}

void TempoOverlay::DrawNoteDivisionSelector(const char* id, bool enabled) noexcept
{
    // Short names in the order of beatMultiples. The long ones say "measures"
    // and do not fit a toolbar; they are kept alongside in the list.
    static constexpr const char* ShortNames[] = {
        "1/1", "1/2", "1/4", "1/8", "1/12", "1/16", "1/24", "1/32", "1/48", "1/64"
    };
    auto& tempo = TempoOverlayState::State(
        OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName));
    const uint32_t current = tempo.measureIndex < beatMultiples.size() ? tempo.measureIndex : 2;

    ImGui::BeginDisabled(!enabled);
    if (ImGui::BeginCombo(id, ShortNames[current], ImGuiComboFlags_HeightLarge)) {
        for (uint32_t i = 0; i < beatMultiples.size(); i += 1) {
            if (ImGui::Selectable(ShortNames[i], i == current)) {
                tempo.measureIndex = i;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal)) {
        if (enabled && tempo.bpm > 0.f) {
            ImGui::SetTooltip("Spacing of the tempo grid's lines: 1/4 is a line every quarter note, "
                "1/8 every eighth note, and so on. At this tempo, a line every %.0f ms.",
                ((60.f * 1000.f) / tempo.bpm) * beatMultiples[current]);
        }
        else {
            ImGui::SetTooltip("%s", "Only used by the Tempo grid. Pick Tempo to set the note spacing.");
        }
    }
}

void DrawPositionRoundingSelector(const char* id) noexcept
{
    // A parameter, so a dropdown: how finely to round, named for what it does.
    static constexpr const char* Names[] = { "Off", "Nearest 5", "Nearest 10", "Nearest 25" };
    static constexpr int32_t Steps[] = { 0, 5, 10, 25 };
    auto& overlayState = BaseOverlay::State();
    int32_t current = 0;
    for (int32_t i = 0; i < 4; i += 1) {
        if (overlayState.SnapPositionStep == Steps[i]) current = i;
    }
    if (ImGui::BeginCombo(id, Names[current])) {
        for (int32_t i = 0; i < 4; i += 1) {
            if (ImGui::Selectable(Names[i], i == current)) overlayState.SnapPositionStep = Steps[i];
        }
        ImGui::EndCombo();
    }
    OFS::Tooltip("Rounds the position, up and down, of points placed or dragged with the mouse: "
                 "off, or to the nearest 5, 10 or 25.");
}

void TempoOverlay::DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& tempo = TempoOverlayState::State(stateHandle);
    followChapterTempo(tempo);
    BaseOverlay::DrawHeightLines(ctx);
    timeline->DrawAudioWaveform(ctx);
    BaseOverlay::DrawSecondsLabel(ctx);

    float beatTime = (60.f / tempo.bpm) * beatMultiples[tempo.measureIndex];
    int32_t visibleBeats = ctx.visibleTime / beatTime;
    int32_t invisiblePreviousBeats = ctx.offsetTime / beatTime;

#ifndef NDEBUG
    static int32_t prevInvisiblePreviousBeats = 0;
    if (prevInvisiblePreviousBeats != invisiblePreviousBeats) {
        LOGF_INFO("%d", invisiblePreviousBeats);
    }
    prevInvisiblePreviousBeats = invisiblePreviousBeats;
#endif

    float offset = -std::fmod(ctx.offsetTime, beatTime) + tempo.beatOffsetSeconds;
    const int lineCount = visibleBeats + 2;
    char tmp[32];

    int32_t lineOffset = tempo.beatOffsetSeconds / beatTime;
    for (int i = -lineOffset; i < lineCount - lineOffset; i += 1) {
        int32_t beatIdx = invisiblePreviousBeats + i;
        const int32_t thing = (int32_t)(1.f / ((beatMultiples[tempo.measureIndex] / 4.f)));
        const bool isWholeMeasure = beatIdx % thing == 0;

        ctx.drawList->AddLine(
            ctx.canvasPos + ImVec2(((offset + (i * beatTime)) / ctx.visibleTime) * ctx.canvasSize.x, 0.f),
            ctx.canvasPos + ImVec2(((offset + (i * beatTime)) / ctx.visibleTime) * ctx.canvasSize.x, ctx.canvasSize.y),
            isWholeMeasure ? beatMultipleColor[tempo.measureIndex] : IM_COL32(255, 255, 255, 153),
            isWholeMeasure ? 5.f : 3.f
        );

        if (isWholeMeasure) {
            stbsp_snprintf(tmp, sizeof(tmp), "%d", thing == 0 ? beatIdx : beatIdx / thing);
            const float textOffsetX = ImGui::GetFontSize() / 2.f;
            ctx.drawList->AddText(OFS_DynFontAtlas::DefaultFont2, ImGui::GetFontSize() * 2.f,
                ctx.canvasPos + ImVec2((((offset + (i * beatTime)) / ctx.visibleTime) * ctx.canvasSize.x) + textOffsetX, 0.f),
                ImGui::GetColorU32(ImGuiCol_Text),
                tmp
            );
        }
    }

    BaseOverlay::DrawActionLines(ctx);
    BaseOverlay::DrawActionPoints(ctx);
}

static float GetNextPosition(float beatTime, float currentTime, float beatOffset) noexcept
{
    float beatIdx = ((currentTime - beatOffset) / beatTime);
    beatIdx = std::floor(beatIdx);

    beatIdx += 1.f;

    float newPosition = (beatIdx * beatTime) + beatOffset;

    if (std::abs(newPosition - currentTime) <= 0.001f) {
        // ugh
        newPosition += beatTime;
    }

    return newPosition;
}

static float GetPreviousPosition(float beatTime, float currentTime, float beatOffset) noexcept
{
    float beatIdx = ((currentTime - beatOffset) / beatTime);
    beatIdx = std::ceil(beatIdx);

    beatIdx -= 1.f;
    float newPosition = (beatIdx * beatTime) + beatOffset;

    if(std::abs(newPosition - currentTime) <= 0.001f) {
        // ugh
        newPosition -= beatTime;
    }

    return newPosition;
}

void TempoOverlay::nextFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& tempo = TempoOverlayState::State(stateHandle);

    float beatTime = (60.f / tempo.bpm) * beatMultiples[tempo.measureIndex];
    float currentTime = app->player->CurrentTime();
    float newPosition = GetNextPosition(beatTime, currentTime, tempo.beatOffsetSeconds);

    app->player->SetPositionExact(newPosition);
}

void TempoOverlay::previousFrame(float realFrameTime) noexcept
{
    auto app = OpenFunscripter::ptr;
    auto& tempo = TempoOverlayState::State(stateHandle);

    float beatTime = (60.f/ tempo.bpm) * beatMultiples[tempo.measureIndex];
    float currentTime = app->player->CurrentTime();
    float newPosition = GetPreviousPosition(beatTime, currentTime, tempo.beatOffsetSeconds);

    app->player->SetPositionExact(newPosition);
}

float TempoOverlay::steppingIntervalForward(float realFrameTime, float fromTime) noexcept
{
    auto& tempo = TempoOverlayState::State(stateHandle);
    float beatTime = (60.f / tempo.bpm) * beatMultiples[tempo.measureIndex];
    return GetNextPosition(beatTime, fromTime, tempo.beatOffsetSeconds) - fromTime;
}

// The same grid the lines are drawn on: n * beatTime + beatOffset. Rounding
// rather than stepping means a point lands on whichever line it is nearest,
// which is what dragging wants, where the stepping functions deliberately
// always move on.
float TempoOverlay::SnapTime(float time) noexcept
{
    auto& tempo = TempoOverlayState::State(stateHandle);
    if(tempo.bpm <= 0.f) return time;
    const float beatTime = (60.f / tempo.bpm) * beatMultiples[tempo.measureIndex];
    if(beatTime <= 0.f) return time;
    const float idx = std::round((time - tempo.beatOffsetSeconds) / beatTime);
    return (idx * beatTime) + tempo.beatOffsetSeconds;
}

const char* TempoOverlay::SnapGridLabel() const noexcept
{
    return TRD(beatMultiplesStrings[TempoOverlayState::State(stateHandle).measureIndex]);
}

float TempoOverlay::steppingIntervalBackward(float realFrameTime, float fromTime) noexcept
{
    auto& tempo = TempoOverlayState::State(stateHandle);
    float beatTime = (60.f / tempo.bpm) * beatMultiples[tempo.measureIndex];
    return GetPreviousPosition(beatTime, fromTime, tempo.beatOffsetSeconds) - fromTime;
}