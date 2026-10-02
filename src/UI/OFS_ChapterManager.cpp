#include "OFS_ChapterManager.h"
#include "state/states/ChapterState.h"
#include "state/ProjectState.h"
#include "OFS_TempoDetection.h"

#include "OpenFunscripter.h"
#include "OFS_EventSystem.h"
#include "OFS_VideoplayerEvents.h"
#include "OFS_ScriptTimelineEvents.h"
#include "OFS_Localization.h"

#include "OFS_ImGui.h"

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "imgui_stdlib.h"

OFS_ChapterManager::OFS_ChapterManager() noexcept
{
    stateHandle = OFS_ProjectState<ChapterState>::Register(ChapterState::StateName);
    tempoStateHandle = OFS_ProjectState<TempoOverlayState>::Register(TempoOverlayState::StateName);

    EV::Queue().appendListener(WaveformProcessingFinishedEvent::EventType,
        WaveformProcessingFinishedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &OFS_ChapterManager::waveformReady)));
}

OFS_ChapterManager::~OFS_ChapterManager() noexcept
{

}

ChapterState& OFS_ChapterManager::State() noexcept
{
    return ChapterState::State(stateHandle);
}

// Shared by the width calculation and the cell that renders them, so the
// column cannot be sized for one string and then asked to hold a longer one.
static constexpr const char* NoMusicLabel = "no music";
static constexpr const char* RowMenuLabel = "...";

// Below this the autocorrelation peak is not really standing out of the noise,
// and the number that comes back is a reading of speech or silence rather than
// of a beat. One threshold, so what a sweep refuses to stamp and what the table
// shows as doubtful are the same judgement.
static constexpr float MinUsableConfidence = 0.35f;

void OFS_ChapterManager::ShowWindow(bool* open) noexcept
{
    if(!*open) return;
    auto& chapterState = ChapterState::State(stateHandle);
    // A size and place for the first time it opens, since the default layout
    // has no slot for it and left to size itself it opened cramped against the
    // left edge. After that, wherever the user puts it is kept.
    {
        const auto* viewport = ImGui::GetMainViewport();
        const float em = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(em * 34.f, em * 20.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    }
    ImGui::Begin(TR_ID("ChapterManager", Tr::CHAPTERS), open);

    // Fetched once for the whole window rather than per row: every control that
    // measures anything needs it, and whether it exists decides which of them
    // can be pressed at all.
    const std::vector<float>* samples = nullptr;
    float envelopeRate = 0.f;
    const bool haveAudio = audioEnvelope(samples, envelopeRate);

    showAnalysisControls(chapterState, samples, envelopeRate, haveAudio);
    showRenameControls(chapterState);
    ImGui::Separator();

    // Every column but the name is fixed width, so the controls keep their
    // space in a narrow panel and the name takes whatever is left. Sizing the
    // controls to content instead let the name crowd them off the edge.
    auto& style = ImGui::GetStyle();
    // Wide enough for the longest thing the tempo cell can say, so the column
    // holds still as chapters are measured and marked rather than resizing
    // under the pointer.
    const float tempoWidth = std::max(ImGui::CalcTextSize("000.0 BPM ?").x, ImGui::CalcTextSize(NoMusicLabel).x)
        + (style.FramePadding.x * 2.f) + (style.CellPadding.x * 2.f);
    const float menuWidth = ImGui::CalcTextSize(RowMenuLabel).x
        + (style.FramePadding.x * 2.f) + (style.CellPadding.x * 2.f);

    // With no chapters the table was a header row over nothing, which says
    // neither what a chapter is for nor how to get one.
    if(chapterState.chapters.empty())
    {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("No chapters yet. Detect tracks and chapters, above, splits the media "
                            "where the music starts, stops or changes tempo, and measures each "
                            "part. A chapter can also be started by hand at the playhead with the "
                            "Create chapter binding, found in Options > Keys.");
        ImGui::PopTextWrapPos();
    }
    else if(ImGui::BeginTable("##chapterTable", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn(TR(CHAPTER), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(TR(TEMPO), ImGuiTableColumnFlags_WidthFixed, tempoWidth);
        ImGui::TableSetupColumn("##menu", ImGuiTableColumnFlags_WidthFixed, menuWidth);
        ImGui::TableHeadersRow();

        bool chapterStateChange = false;
        int deleteIdx = -1;
        for(int i=0, size=chapterState.chapters.size(); i < size; i += 1)
        {
            auto& chapter = chapterState.chapters[i];
            ImGui::PushID(i);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::ColorEdit3("##chapterColorPicker", &chapter.color.Value.x, ImGuiColorEditFlags_NoInputs);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            chapterStateChange |= ImGui::InputText("##chapterName", &chapter.name);
            // The times are reference, not something to read at a glance: the
            // chapter is already drawn on the timeline where it belongs. They
            // are here for when an exact figure is wanted.
            if(ImGui::IsItemHovered() && !ImGui::IsItemActive()) {
                char startBuf[16];
                char endBuf[16];
                Util::FormatTime(startBuf, sizeof(startBuf), chapter.startTime, true);
                Util::FormatTime(endBuf, sizeof(endBuf), chapter.endTime, true);
                ImGui::SetTooltip("%s - %s  (%.1fs)", startBuf, endBuf,
                    chapter.endTime - chapter.startTime);
            }

            ImGui::TableNextColumn();
            // One cell for one question. A break and a tempo were two columns
            // that could never both be live -- a chapter marked as a break
            // showed a dash where its tempo would go -- because they are the
            // same fact about the chapter. So they are one control: the label
            // reads the state, and measuring is the only verb it has.
            const bool wasBreak = chapter.isBreak;
            const bool doubtful = !wasBreak && !chapter.bpmManual && chapter.bpm > 0.f
                && chapter.bpmConfidence > 0.f && chapter.bpmConfidence < MinUsableConfidence;
            char tempoLabel[24];
            if(wasBreak) stbsp_snprintf(tempoLabel, sizeof(tempoLabel), "%s", NoMusicLabel);
            // A tempo read off a passage with no steady beat is still a number
            // and looks exactly like a good one, so a doubtful reading says so
            // on its face. A mark rather than a colour: the palette here is
            // greys and one pink, and neither of them means "careful".
            else if(chapter.bpm > 0.f) stbsp_snprintf(tempoLabel, sizeof(tempoLabel),
                chapter.bpmManual ? "%.1f BPM *" : (doubtful ? "%.1f BPM ?" : "%.1f BPM"), chapter.bpm);
            else stbsp_snprintf(tempoLabel, sizeof(tempoLabel), "measure");

            if(wasBreak) ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_TextDisabled]);
            const bool measureClicked = ImGui::Button(tempoLabel, ImVec2(-1.f, 0.f));
            if(wasBreak) ImGui::PopStyleColor();

            if(measureClicked) {
                // Measuring a chapter marked as having no music is an assertion
                // that it has some, so the mark comes off rather than the click
                // being refused with nothing to show for it.
                chapter.isBreak = false;
                chapter.bpmManual = false;
                chapterStateChange = true;
                // Stamped where it was asked for, rather than parked in a
                // result panel waiting for a second click on an Apply button
                // that lived in a section collapsed by default. Pressing this
                // with the section shut used to do nothing visible at all.
                if(haveAudio) measureChapter(chapter, *samples, envelopeRate);
                else requestWaveformThen(i);
            }
            if(wasBreak) {
                OFS::Tooltip("Marked as having no music. Click to measure it anyway, which clears the mark.");
            }
            else if(chapter.bpmManual) {
                OFS::Tooltip("* Set by hand. Measure all chapters leaves it alone. Click to "
                             "measure this chapter from the audio instead, which replaces it.");
            }
            else if(chapter.bpm > 0.f) {
                char scaleNote[64] = "";
                if(chapter.tempoScale > 1.001f || chapter.tempoScale < 0.999f) {
                    stbsp_snprintf(scaleNote, sizeof(scaleNote), "\n%s: measured as %.1f BPM.",
                        chapter.tempoScale > 1.f ? "Double time" : "Half time",
                        chapter.bpm / chapter.tempoScale);
                }
                OFS::TooltipFmt("Confidence %.0f%%.%s%s\nClick to measure again. Detection trims a "
                                "non-musical intro and outro first, so the downbeat need not be "
                                "where the chapter begins.",
                    chapter.bpmConfidence * 100.f,
                    doubtful ? " Low: the audio may not have a steady beat." : "",
                    scaleNote);
            }
            else {
                OFS::Tooltip("Measure this chapter's tempo from the audio waveform.");
            }

            // Right clicking the tempo opens the row's menu, which is where
            // marking a chapter as having no music lives. One menu per row
            // rather than one per cell: a row with a context menu on some of
            // its cells and not others is a worse guessing game than a row with
            // one menu reachable from either end.
            bool openRowMenu = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right);

            ImGui::TableNextColumn();
            if(ImGui::Button(RowMenuLabel)) openRowMenu = true;
            OFS::Tooltip("Chapter actions");

            if(openRowMenu) ImGui::OpenPopup("##chapterMenu");
            if(ImGui::BeginPopup("##chapterMenu"))
            {
                auto player = OpenFunscripter::ptr->player.get();
                const float currentTime = player->CurrentTime();

                if(ImGui::MenuItem(TR(GO_TO_CHAPTER))) {
                    player->SetPositionExact(chapter.startTime);
                }

                ImGui::Separator();

                // The same two verbs the timeline's own chapter menu offers.
                // The table could name a chapter and measure it but not move
                // it, while the timeline could move it but knew nothing about
                // tempo, so which window you were in decided what you were
                // allowed to do.
                if(ImGui::MenuItem(TR(SET_CHAPTER_START))) {
                    if(chapterState.SetChapterStart(chapter, currentTime)) chapterStateChange = true;
                }
                OFS::Tooltip("Moves this chapter's start to the playhead, carrying the end of "
                             "the chapter before it along when the two meet.");

                if(ImGui::MenuItem(TR(SET_CHAPTER_END))) {
                    if(chapterState.SetChapterEnd(chapter, currentTime)) chapterStateChange = true;
                }
                OFS::Tooltip("Moves this chapter's end to the playhead, carrying the start of "
                             "the chapter after it along when the two meet.");

                ImGui::Separator();

                // Whether an intro is an intro is not something a waveform can
                // settle, so marking it stays a hand operation. It just no
                // longer costs a column of its own to do it.
                if(ImGui::MenuItem(TR(NO_MUSIC_HERE), nullptr, chapter.isBreak)) {
                    chapter.isBreak = !chapter.isBreak;
                    if(chapter.isBreak) {
                        chapter.bpm = 0.f;
                        chapter.measureOffsetSeconds = 0.f;
                        chapter.bpmConfidence = 0.f;
                    }
                    chapterStateChange = true;
                }
                OFS::Tooltip("An intro, an outro, or a break between songs. Detection skips it "
                             "and the grid holds the tempo it already had.");

                // Detection counts the pulse it hears most strongly, which for a
                // lot of music is half or double the beat that is wanted. Either
                // way is a click, and it is remembered on the chapter, so
                // measuring again keeps the correction.
                if(ImGui::MenuItem("Double time (x2)", nullptr, false, chapter.bpm > 0.f && chapter.bpm * 2.f <= 1000.f)) {
                    chapter.ScaleTempo(2.f);
                    chapterStateChange = true;
                }
                OFS::TooltipFmt("%.1f BPM becomes %.1f, for a tempo read off a pulse half as fast as "
                                "the beat. Kept when the chapter is measured again.",
                    chapter.bpm, chapter.bpm * 2.f);
                if(ImGui::MenuItem("Half time (/2)", nullptr, false, chapter.bpm > 0.f && chapter.bpm / 2.f >= 1.f)) {
                    chapter.ScaleTempo(0.5f);
                    chapterStateChange = true;
                }
                OFS::TooltipFmt("%.1f BPM becomes %.1f, for a tempo read off a pulse twice as fast as "
                                "the beat. Kept when the chapter is measured again.",
                    chapter.bpm, chapter.bpm * 0.5f);

                // A tempo typed by hand, for when detection cannot find the beat or
                // finds the wrong one and neither half nor double is right.
                {
                    float typed = chapter.bpm > 0.f ? chapter.bpm : 120.f;
                    if(OFS::StepperFloat("Set tempo (BPM)", "##chapterBpm", &typed, 1.f, 1.f, 1000.f, "%.1f")) {
                        chapter.SetTempoByHand(typed);
                        chapterStateChange = true;
                    }
                    OFS::Tooltip("Type this chapter's tempo. Measure all chapters then leaves it alone.");
                }
                if(ImGui::MenuItem("Keep this tempo", nullptr, chapter.bpmManual, chapter.bpm > 0.f)) {
                    chapter.bpmManual = !chapter.bpmManual;
                    chapterStateChange = true;
                }
                OFS::Tooltip("Measure all chapters leaves a kept tempo alone. Measuring this chapter "
                             "on its own still replaces it.");

                // Only of use when the grid is not following chapters by
                // itself. With auto-follow on it is what happens anyway as soon
                // as the playhead is inside this chapter.
                if(ImGui::MenuItem("Apply to tempo grid", nullptr, false, chapter.bpm > 0.f)) {
                    applyChapterTempoToGrid(chapter);
                }
                OFS::Tooltip("Sets the tempo grid from this chapter's BPM, for when the grid is "
                             "not set to follow chapters on its own.");

                ImGui::Separator();

                if(ImGui::MenuItem(TR(EXPORT_CLIP))) {
                    EV::Enqueue<ExportClipForChapter>(chapter);
                }

                if(ImGui::MenuItem(TR(REMOVE))) {
                    deleteIdx = i;
                    chapterStateChange = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        if(deleteIdx >= 0 && deleteIdx < chapterState.chapters.size())
        {
            auto it = chapterState.chapters.begin() + deleteIdx;
            chapterState.chapters.erase(it);
        }
        
        if(chapterStateChange)
            EV::Enqueue<ChapterStateChanged>();

        ImGui::EndTable();
    }

    ImGui::End();
}

// The timeline waveform is already a decoded amplitude envelope, so tempo
// analysis reuses it instead of decoding the audio a second time. Its rate is
// derived from the sample count rather than stored, because the source rate
// depends on whatever ffmpeg produced.
bool OFS_ChapterManager::audioEnvelope(const std::vector<float>*& outSamples, float& outRate) noexcept
{
    auto app = OpenFunscripter::ptr;
    const auto& samples = app->scriptTimeline.Wave.data.Samples();
    const float duration = app->player->Duration();
    if(samples.empty() || duration <= 0.f) return false;

    outSamples = &samples;
    outRate = (float)samples.size() / duration;
    return true;
}

void OFS_ChapterManager::applyTempoToGrid(const OFS_Tempo::TempoResult& tempo) noexcept
{
    if(!tempo.valid || tempo.bpm <= 0.f) return;
    // Anchors on a downbeat rather than just any beat: the grid measures in
    // whole bars by default, so a beat aligned offset can sit up to three beats
    // out. The reduction into one bar lives on the state itself, so the manual
    // apply here and the automatic mode in the overlay cannot drift apart.
    TempoOverlayState::State(tempoStateHandle).SetFromTempo(tempo.bpm, tempo.measureOffsetSeconds);
}

// Records a detected tempo on the chapter it was measured from, which is what
// the grid's automatic mode reads back as the playhead moves.
void OFS_ChapterManager::stampTempoOnChapter(Chapter& chapter, const OFS_Tempo::TempoResult& tempo) noexcept
{
    if(!tempo.valid || tempo.bpm <= 0.f) return;
    // With the chapter's own correction, so a chapter set to double time stays
    // in double time however often it is measured.
    chapter.bpm = tempo.bpm * (chapter.tempoScale > 0.f ? chapter.tempoScale : 1.f);
    chapter.measureOffsetSeconds = tempo.measureOffsetSeconds;
    chapter.bpmConfidence = tempo.confidence;
}

// Applies a chapter's recorded tempo to the grid. The verb lives on the row
// rather than in a section of its own: the thing being applied is the number in
// that cell, and there was no reason for the button to be anywhere else.
void OFS_ChapterManager::applyChapterTempoToGrid(const Chapter& chapter) noexcept
{
    if(chapter.bpm <= 0.f) return;
    TempoOverlayState::State(tempoStateHandle).SetFromTempo(chapter.bpm, chapter.measureOffsetSeconds);
}

// Takes whatever comes back, low confidence included: this chapter was asked
// about by name, and the answer is marked as doubtful rather than withheld. A
// sweep is the cautious one, because it must not overwrite a correction made by
// hand.
void OFS_ChapterManager::measureChapter(Chapter& chapter,
    const std::vector<float>& samples, float envelopeRate) noexcept
{
    auto tempo = OFS_Tempo::Detect(samples, envelopeRate, chapter.startTime, chapter.endTime);
    lastAnalysis = AnalysisResult();
    lastAnalysis.ran = true;
    if(tempo.valid && tempo.bpm > 0.f) {
        stampTempoOnChapter(chapter, tempo);
        lastAnalysis.measured = 1;
    }
    else {
        // Reported rather than leaving the cell to fall back to "measure" as
        // though the click had never landed.
        lastAnalysis.unreadable = 1;
    }
}

// Asks for the waveform and remembers what to do when it arrives. Detection
// cannot run without the envelope, and nobody who presses detect wants to be
// sent to a menu in another window to make one and then come back. The reverse
// is not true -- a waveform is worth having on its own, to see the beats while
// scripting -- so the timeline keeps its own entry for it.
void OFS_ChapterManager::requestWaveformThen(int32_t chapterIdx) noexcept
{
    pendingChapter = chapterIdx;
    pendingAnalysis = true;
    OpenFunscripter::ptr->scriptTimeline.RequestWaveform();
}

void OFS_ChapterManager::waveformReady(const WaveformProcessingFinishedEvent* ev) noexcept
{
    // The waveform is generated for its own sake far more often than for this,
    // so an arrival nobody here asked for is the normal case.
    if(!pendingAnalysis) return;
    pendingAnalysis = false;
    const int32_t chapterIdx = pendingChapter;
    pendingChapter = -1;

    const std::vector<float>* samples = nullptr;
    float rate = 0.f;
    if(!audioEnvelope(samples, rate)) {
        // Generation can finish with nothing to show for it: no ffmpeg, no
        // audio track, a codec it could not read. Better said than left as a
        // button that looks like it was never pressed.
        lastAnalysis = AnalysisResult();
        lastAnalysis.ran = true;
        lastAnalysis.audioFailed = true;
        return;
    }

    auto& chapterState = ChapterState::State(stateHandle);
    if(chapterIdx >= 0) {
        if(chapterIdx < (int32_t)chapterState.chapters.size()) {
            measureChapter(chapterState.chapters[chapterIdx], *samples, rate);
            EV::Enqueue<ChapterStateChanged>();
        }
    }
    else if(chapterState.chapters.empty()) {
        createChaptersFromTracks(chapterState, *samples, rate);
    }
    else {
        measureAllChapters(chapterState, *samples, rate);
        EV::Enqueue<ChapterStateChanged>();
    }
}

// Measures every chapter the user has laid out.
//
// Hand placed chapters are better evidence than anything measured: the
// boundaries are where they are because someone heard them there. This measures
// each one over exactly those bounds, so the way to a good grid is to mark the
// structure once and press the button.
void OFS_ChapterManager::measureAllChapters(ChapterState& chapterState,
    const std::vector<float>& samples, float envelopeRate) noexcept
{
    lastAnalysis = AnalysisResult();
    for(size_t i = 0; i < chapterState.chapters.size(); i += 1) {
        auto& chapter = chapterState.chapters[i];
        // Nor one whose tempo was set by hand, which is a correction a sweep
        // over the whole table must not be able to throw away.
        if(chapter.isBreak || chapter.bpmManual) {
            lastAnalysis.skipped += 1;
            continue;
        }
        auto tempo = OFS_Tempo::Detect(samples, envelopeRate, chapter.startTime, chapter.endTime);
        if(tempo.valid && tempo.confidence >= MinUsableConfidence) {
            stampTempoOnChapter(chapter, tempo);
            lastAnalysis.measured += 1;
        }
        else {
            // Left alone rather than stamped with a guess. A chapter whose
            // tempo could not be read keeps whatever it had, which matters most
            // for one that was corrected by hand: a sweep over the whole table
            // must not be able to throw that away. Measuring a single chapter
            // takes whatever comes back, because that was asked for by name.
            lastAnalysis.unreadable += 1;
        }
    }
    lastAnalysis.ran = true;
}

// Splits the audio into its tracks and lays a chapter over each one.
//
// The segments used to be listed in a table of their own with a button per row,
// which meant the same structure was shown twice: once as a proposal and again
// as the chapters it became, with the proposal going stale the moment a
// boundary was dragged. Chapters are the only view of the structure now, and
// correcting one is a drag on a boundary rather than a re-run.
void OFS_ChapterManager::createChaptersFromTracks(ChapterState& chapterState,
    const std::vector<float>& samples, float envelopeRate) noexcept
{
    lastAnalysis = AnalysisResult();
    lastAnalysis.ran = true;

    auto tracks = OFS_Tempo::DetectSegments(samples, envelopeRate);
    if(tracks.empty()) return;

    int32_t trackNumber = 0;
    for(size_t i = 0; i < tracks.size(); i += 1) {
        const auto& track = tracks[i];
        // Breaks get a chapter too. They carry no tempo and the grid ignores
        // them, but laying the whole structure out is what makes it
        // correctable: a boundary in the wrong place can be dragged, where a
        // missing one has to be created from nothing.
        if(track.hasMusic) trackNumber += 1;

        bool overlaps = false;
        for(size_t c = 0; c < chapterState.chapters.size(); c += 1) {
            const auto& existing = chapterState.chapters[c];
            // Half open, matching checkForOverlapChapters. Sharing a boundary
            // is not an overlap, and testing it as one here threw away every
            // second segment the moment they were made to touch: each new
            // chapter met the one before it and was refused.
            if(track.startTime < existing.endTime && existing.startTime < track.endTime) {
                overlaps = true;
                break;
            }
        }
        if(overlaps) {
            lastAnalysis.skipped += 1;
            continue;
        }

        Chapter chapter;
        chapter.startTime = track.startTime;
        chapter.endTime = track.endTime;
        chapter.color = Util::RandomColor(0.65f, 0.70f);

        char name[64];
        if(!track.hasMusic) {
            // Named for where it falls, since that is what these are: the one
            // before all the music is the intro and the one after all of it is
            // the outro. Only a name, though. The break mark stays off because
            // whether the music really stopped is still a question for the ear:
            // a beat carrying on quietly under speech reads the same way as
            // silence does.
            const bool first = (i == 0);
            const bool last = (i + 1 == tracks.size());
            if(first && !last) stbsp_snprintf(name, sizeof(name), "Intro");
            else if(last && !first) stbsp_snprintf(name, sizeof(name), "Outro");
            else stbsp_snprintf(name, sizeof(name), "Break");
            // Skipped, not unreadable. A stretch with no music in it is the
            // expected outcome for an intro, and counting it as a failure to
            // read would make every successful detection look half broken.
            lastAnalysis.skipped += 1;
        }
        else if(track.tempo.valid) {
            stbsp_snprintf(name, sizeof(name), "Track %d - %.0f BPM", (int)trackNumber, track.tempo.bpm);
            stampTempoOnChapter(chapter, track.tempo);
            lastAnalysis.measured += 1;
        }
        else {
            stbsp_snprintf(name, sizeof(name), "Track %d", (int)trackNumber);
            lastAnalysis.unreadable += 1;
        }
        chapter.name = name;

        chapterState.chapters.emplace_back(std::move(chapter));
        lastAnalysis.created += 1;
    }

    std::sort(chapterState.chapters.begin(), chapterState.chapters.end(),
        [](const Chapter& a, const Chapter& b) noexcept { return a.startTime < b.startTime; });

    if(lastAnalysis.created > 0) EV::Enqueue<ChapterStateChanged>();
}

// The name each chapter would get from where it falls and what it holds, as
// detection names what it creates: music is "Track N - 120 BPM", numbered in
// order along the timeline, or "Track N" before it is measured; a stretch with
// no music is the Intro before all of it, the Outro after all of it, and a
// Break between. A chapter counts as having no music when it is marked so, or
// when it has no tempo and is still called Intro, Outro or Break, which is how
// detection leaves one it did not mark.
void OFS_ChapterManager::autoChapterNames(const ChapterState& chapterState, std::vector<std::string>& out) noexcept
{
    const auto& chapters = chapterState.chapters;
    out.assign(chapters.size(), std::string());
    if(chapters.empty()) return;

    std::vector<size_t> order(chapters.size());
    for(size_t i = 0; i < order.size(); i += 1) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) noexcept {
        return chapters[a].startTime < chapters[b].startTime;
    });

    auto noMusic = [](const Chapter& chapter) noexcept {
        if(chapter.isBreak) return true;
        if(chapter.bpm > 0.f) return false;
        return chapter.name == "Intro" || chapter.name == "Outro" || chapter.name == "Break";
    };

    int32_t firstMusic = -1;
    int32_t lastMusic = -1;
    for(int32_t k = 0; k < (int32_t)order.size(); k += 1) {
        if(noMusic(chapters[order[k]])) continue;
        if(firstMusic < 0) firstMusic = k;
        lastMusic = k;
    }

    int32_t trackNumber = 0;
    char name[64];
    for(int32_t k = 0; k < (int32_t)order.size(); k += 1) {
        const auto& chapter = chapters[order[k]];
        if(noMusic(chapter)) {
            const bool beforeAll = firstMusic < 0 ? k == 0 : k < firstMusic;
            const bool afterAll = firstMusic < 0 ? k + 1 == (int32_t)order.size() : k > lastMusic;
            if(beforeAll && !(firstMusic < 0 && afterAll && order.size() > 1)) stbsp_snprintf(name, sizeof(name), "Intro");
            else if(afterAll) stbsp_snprintf(name, sizeof(name), "Outro");
            else stbsp_snprintf(name, sizeof(name), "Break");
        }
        else {
            trackNumber += 1;
            if(chapter.bpm > 0.f) stbsp_snprintf(name, sizeof(name), "Track %d - %.0f BPM", (int)trackNumber, chapter.bpm);
            else stbsp_snprintf(name, sizeof(name), "Track %d", (int)trackNumber);
        }
        out[order[k]] = name;
    }
}

// Renaming every chapter at once replaces names that may have been typed by
// hand, and chapters are not undoable, so it asks first, showing exactly
// which names change and to what.
void OFS_ChapterManager::showRenameControls(ChapterState& chapterState) noexcept
{
    if(chapterState.chapters.empty()) return;

    constexpr const char* PopupId = "Rename chapters###renameChapters";
    if(ImGui::Button("Rename chapters...", ImVec2(-1.f, 0.f))) {
        autoChapterNames(chapterState, renamePreview);
        ImGui::OpenPopup(PopupId);
    }
    OFS::Tooltip("Names every chapter from where it falls and its tempo: Track 1 - 120 BPM, "
                 "Intro, Outro or Break. Shows what will change before anything does.");

    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if(!ImGui::BeginPopupModal(PopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    // The chapters can change while this is open, from the timeline or a
    // measurement finishing, so what is shown is kept in step with them.
    if(renamePreview.size() != chapterState.chapters.size()) {
        autoChapterNames(chapterState, renamePreview);
    }
    int32_t changed = 0;
    for(size_t i = 0; i < renamePreview.size(); i += 1) {
        if(chapterState.chapters[i].name != renamePreview[i]) changed += 1;
    }

    const float em = ImGui::GetFontSize();
    if(changed == 0) {
        ImGui::TextUnformatted("Every chapter already has the name it would get.");
        ImGui::Spacing();
        if(ImGui::Button("Close", ImVec2(em * 8.f, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Rename %d chapter%s?", (int)changed, changed == 1 ? "" : "s");
    ImGui::TextDisabled("Names typed by hand are replaced too. This cannot be undone.");
    ImGui::Spacing();

    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().CellPadding.y * 2.f;
    const float listHeight = std::min((float)changed + 1.f, 12.f) * rowHeight;
    if(ImGui::BeginTable("##renamePreview", 2,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
        ImVec2(em * 30.f, listHeight))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Now");
        ImGui::TableSetupColumn("Becomes");
        ImGui::TableHeadersRow();
        for(size_t i = 0; i < renamePreview.size(); i += 1) {
            const auto& chapter = chapterState.chapters[i];
            if(chapter.name == renamePreview[i]) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if(chapter.name.empty()) ImGui::TextDisabled("(no name)");
            else ImGui::TextUnformatted(chapter.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(renamePreview[i].c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const float buttonWidth = em * 8.f;
    ImGui::PushStyleColor(ImGuiCol_Button, OFS_Sashimi::V4(OFS_Sashimi::PinkFill));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, OFS_Sashimi::V4(OFS_Sashimi::PinkFillHi));
    const bool rename = ImGui::Button("Rename", ImVec2(buttonWidth, 0.f));
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(buttonWidth, 0.f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);

    if(rename) {
        for(size_t i = 0; i < renamePreview.size(); i += 1) {
            chapterState.chapters[i].name = renamePreview[i];
        }
        EV::Enqueue<ChapterStateChanged>();
        ImGui::CloseCurrentPopup();
    }
    else if(cancel) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// One button, because there is only ever one sensible thing to do with the
// audio: find the structure when there is none, and measure the structure that
// is there. Three buttons that all said "detect" left it to the user to work
// out which of them was meant for their situation.
void OFS_ChapterManager::showAnalysisControls(ChapterState& chapterState,
    const std::vector<float>* samples, float envelopeRate, bool haveAudio) noexcept
{
    const bool haveChapters = !chapterState.chapters.empty();
    auto& timeline = OpenFunscripter::ptr->scriptTimeline;
    const bool working = pendingAnalysis || timeline.WaveformBusy();
    // Missing the waveform is no longer a reason to refuse: the button makes
    // one and carries on. There is no case where someone presses detect and
    // does not want the waveform it needs, so being told to go and make it
    // elsewhere was a step that existed only because the code was in two
    // places.
    const bool canRun = haveAudio || timeline.CanGenerateWaveform();

    ImGui::BeginDisabled(working || !canRun);
    const char* label = haveChapters ? "Measure all chapters" : "Detect tracks and chapters";
    if(ImGui::Button(label, ImVec2(-1.f, 0.f))) {
        if(!haveAudio) requestWaveformThen(-1);
        else if(haveChapters) measureAllChapters(chapterState, *samples, envelopeRate);
        else createChaptersFromTracks(chapterState, *samples, envelopeRate);
    }
    ImGui::EndDisabled();

    if(working) {
        // Said in the panel rather than in a tooltip, because a disabled button
        // does not report hover and the explanation would never be read.
        OFS::Spinner("##chapterAudioSpin", ImGui::GetFontSize() / 3.f, 4.f,
            ImGui::GetColorU32(ImGuiCol_TabActive));
        ImGui::SameLine();
        ImGui::TextDisabled("Reading the audio...");
        return;
    }

    if(!canRun) {
        ImGui::TextDisabled("Load a video or audio file first.");
        return;
    }

    OFS::Tooltip(haveChapters
        ? "Measures every chapter that is not marked as having no music, over the bounds "
          "you set. A chapter whose tempo cannot be read is left as it was."
        : "Scans the whole audio, splits it where the tempo changes -- usually one track "
          "ending and the next starting -- and lays a chapter over each stretch.");

    if(!lastAnalysis.ran) return;

    if(lastAnalysis.audioFailed) {
        ImGui::TextWrapped("Could not read the audio. Check that ffmpeg is available and that "
                           "the media actually has an audio track.");
        return;
    }

    // Only the counts that are not zero. Every measuring action in the panel
    // reports here -- one chapter, every chapter, or a whole detection -- so
    // the line has to read properly for all of them, and "0 measured, 0
    // skipped, 1 could not be read" is not a sentence anyone wants to parse.
    char status[160];
    int len = 0;
    auto append = [&](const char* fmt, int32_t count) noexcept {
        if(count <= 0) return;
        if(len > 0 && len < (int)sizeof(status)) {
            len += stbsp_snprintf(status + len, sizeof(status) - len, ", ");
        }
        if(len < (int)sizeof(status)) {
            len += stbsp_snprintf(status + len, sizeof(status) - len, fmt, count);
        }
    };
    append("%d chapter(s) created", lastAnalysis.created);
    append("%d measured", lastAnalysis.measured);
    append("%d skipped", lastAnalysis.skipped);
    append("%d could not be read", lastAnalysis.unreadable);

    if(len == 0) {
        ImGui::TextDisabled("Nothing found. The audio may have no steady beat.");
    }
    else {
        ImGui::TextDisabled("%s", status);
    }
}

bool OFS_ChapterManager::ExportClip(const Chapter& chapter, const std::string& outputDirStr) noexcept
{
    auto app = OpenFunscripter::ptr;
    char startTimeChar[16];
    char endTimeChar[16];
    stbsp_snprintf(startTimeChar, sizeof(startTimeChar), "%f", chapter.startTime);
    stbsp_snprintf(endTimeChar, sizeof(endTimeChar), "%f", chapter.endTime);

    
    auto outputDir = Util::PathFromString(outputDirStr);
    if (app->player->IsBlank()) {
        // Nothing to cut a clip out of.
        Util::MessageBoxAlert("No media", "This project has no video or audio to export a clip from.");
        return false;
    }
    auto mediaPath = Util::PathFromString(app->player->VideoPath());

    auto& projectState = app->LoadedProject->State();

    for(auto& script : app->LoadedFunscripts())
    {
        auto scriptOutputPath = (outputDir / (chapter.name + "_" + script->Title()));
        scriptOutputPath.replace_extension(".funscript");
        auto scriptOutputPathStr = scriptOutputPath.u8string();

        auto clippedScript = Funscript();
        auto slice = script->GetSelection(chapter.startTime, chapter.endTime);
        clippedScript.SetActions(slice);
        clippedScript.AddEditAction(FunscriptAction(chapter.startTime, script->GetPositionAtTime(chapter.startTime)), 0.001f);
        clippedScript.AddEditAction(FunscriptAction(chapter.endTime, script->GetPositionAtTime(chapter.endTime)), 0.001f);
        clippedScript.SelectAll();
        clippedScript.MoveSelectionTime(-chapter.startTime, 0.f);

        // FIXME: chapters and bookmarks are not included
        auto funscriptJson = clippedScript.Serialize(projectState.metadata, false);
        auto funscriptText = Util::SerializeJson(funscriptJson);
        Util::WriteFile(scriptOutputPathStr.c_str(), funscriptText.data(), funscriptText.size());
    }

    auto clippedMedia = Util::PathFromString("");
    clippedMedia.replace_filename(chapter.name + "_" + mediaPath.filename().u8string());
    clippedMedia.replace_extension(mediaPath.extension());
    
    auto videoOutputPath = outputDir / clippedMedia;
    auto videoOutputString = videoOutputPath.u8string();
    
    auto ffmpegPath = Util::FfmpegPath().u8string();
    auto mediaPathStr = mediaPath.u8string();

    std::array<const char*, 17> args = {
        ffmpegPath.c_str(),
        "-y",
        "-ss", startTimeChar,
        "-to", endTimeChar,
        "-i", mediaPathStr.c_str(),
        "-vcodec", "copy",
        "-acodec", "copy",
        videoOutputString.c_str(),
        nullptr
    };

    struct subprocess_s proc;
    if (subprocess_create(args.data(), subprocess_option_no_window, &proc) != 0) {
        return false;
    }

    if (proc.stdout_file) {
        fclose(proc.stdout_file);
        proc.stdout_file = nullptr;
    }

    if (proc.stderr_file) {
        fclose(proc.stderr_file);
        proc.stderr_file = nullptr;
    }

    int returnCode;
    subprocess_join(&proc, &returnCode);
    subprocess_destroy(&proc);

    return returnCode == 0;
}