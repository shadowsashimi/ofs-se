#pragma once
#include "OFS_StateHandle.h"

#include <algorithm>
#include <vector>
#include <string>

#include "nlohmann/json.hpp"
#include "OFS_Event.h"

struct Chapter
{
    float startTime = 0.f;
    float endTime = 0.f;
    std::string name;
    ImColor color = IM_COL32(0x8C, 0x2B, 0x51, 255);
    // Tempo of this chapter, stamped by detection. Zero means unknown, which is
    // what a chapter created by hand carries. Kept per chapter so the grid can
    // follow the playhead across a mix without being reapplied at every track.
    // Project state only: the funscript writer lists chapter fields explicitly,
    // so these do not reach the exported file.
    float bpm = 0.f;
    float measureOffsetSeconds = 0.f;
    // How much the detector trusted the reading that produced bpm, 0..1. Kept
    // so the number in the table can say how much to believe it: a tempo taken
    // off a passage with no steady beat is still a number, and looks exactly
    // like a good one without this.
    float bpmConfidence = 0.f;
    // A correction to the measured tempo, by a power of two. Detection counts
    // the pulse it hears most strongly, which is often half or double the beat
    // the music is felt in: a 120 BPM track with its kick on every other beat
    // reads as 60. 2 counts it double, 0.5 half. Kept on the chapter, so
    // measuring it again lands on the corrected tempo rather than undoing it.
    float tempoScale = 1.f;
    // The tempo was set by hand. Measuring every chapter leaves it alone, since
    // someone who typed a tempo knows better than the detector; measuring this
    // chapter on its own is asking for the audio's answer, and clears it.
    bool bpmManual = false;
    // Marked by hand for the stretches with no music in them: an intro, an
    // outro, a break between songs. Detection skips these, and the grid holds
    // whatever tempo it had rather than trying to measure speech. Intro and
    // outro are not separate kinds because they behave identically; what they
    // are called is what the name is for.
    bool isBreak = false;
    nlohmann::json unknownFields; // Preserve unrecognized fields from funscript

    // Double time with 2, half time with 0.5. The downbeat stays where it
    // was: every bar line of the old grid is a bar line of the new one.
    inline void ScaleTempo(float factor) noexcept
    {
        if(bpm <= 0.f || factor <= 0.f) return;
        bpm *= factor;
        // A tempo typed by hand has no measurement for a correction to apply to.
        if(bpmManual) return;
        const float scale = (tempoScale > 0.f ? tempoScale : 1.f) * factor;
        tempoScale = std::min(std::max(scale, 0.125f), 8.f);
    }

    inline void SetTempoByHand(float newBpm) noexcept
    {
        if(newBpm <= 0.f) return;
        bpm = newBpm;
        bpmManual = true;
        isBreak = false;
    }

    std::string StartTimeToString() const noexcept;
    std::string EndTimeToString() const noexcept;
};

struct Bookmark
{
    float time = 0.f;
    std::string name;
    nlohmann::json unknownFields; // Preserve unrecognized fields from funscript

    std::string TimeToString() const noexcept;
};

struct ChapterState
{
    static constexpr auto StateName = "ChapterState";
    std::vector<Chapter> chapters;
    std::vector<Bookmark> bookmarks;

    inline static ChapterState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<ChapterState>(stateHandle).Get();
    }

    inline static ChapterState& StaticStateSlow() noexcept
    {
        auto handle = OFS_ProjectState<ChapterState>::Register(StateName);
        return State(handle);
    }

    bool SetChapterSize(Chapter& chapter, float toTime) noexcept;
    // Explicit about which edge is being moved. SetChapterSize picks whichever
    // is nearer, which is a guess, and one that cannot be overruled when the
    // edge that is wanted happens to be the far one.
    bool SetChapterStart(Chapter& chapter, float toTime) noexcept;
    bool SetChapterEnd(Chapter& chapter, float toTime) noexcept;
    Chapter* AddChapter(float time, float duration) noexcept;
    Bookmark* AddBookmark(float time) noexcept;
};

REFL_TYPE(Chapter)
    REFL_FIELD(startTime)
    REFL_FIELD(endTime)
    REFL_FIELD(name)
    REFL_FIELD(color)
    REFL_FIELD(bpm)
    REFL_FIELD(measureOffsetSeconds)
    REFL_FIELD(bpmConfidence)
    REFL_FIELD(tempoScale)
    REFL_FIELD(bpmManual)
    REFL_FIELD(isBreak)
REFL_END

REFL_TYPE(Bookmark)
    REFL_FIELD(time)
    REFL_FIELD(name)
REFL_END

REFL_TYPE(ChapterState)
    REFL_FIELD(chapters)
    REFL_FIELD(bookmarks)
REFL_END


class ChapterStateChanged : public OFS_Event<ChapterStateChanged>
{
    public:
    ChapterStateChanged() noexcept {}
};

class ExportClipForChapter : public OFS_Event<ExportClipForChapter>
{
    public:
    Chapter chapter;
    ExportClipForChapter(const Chapter& chapter) noexcept
        : chapter(chapter) {}
};