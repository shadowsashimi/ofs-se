#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "OFS_TempoDetection.h"

// Declared as struct, matching ChapterState.h. MSVC mangles an elaborated
// "class Chapter" differently from "struct Chapter", so naming it one way here
// and the other way in the definition costs a link error.
struct Chapter;
struct ChapterState;

class OFS_ChapterManager
{
    private:
    uint32_t stateHandle = 0xFFFF'FFFF;
    uint32_t tempoStateHandle = 0xFFFF'FFFF;

    // Outcome of the last thing the analyse button did, so it can say what it
    // did rather than leaving the user to compare BPM columns before and after.
    // A single struct for both operations because both answer the same
    // question: what changed in the table just now.
    struct AnalysisResult
    {
        int32_t created = 0;
        int32_t measured = 0;
        int32_t skipped = 0;
        int32_t unreadable = 0;
        bool audioFailed = false;
        bool ran = false;
    };
    AnalysisResult lastAnalysis;

    // Set while waiting for a waveform that was asked for in order to run an
    // analysis. Generation is a background thread, so the button press and the
    // work it leads to are separated by however long ffmpeg takes.
    // pendingChapter is the row that asked, or -1 for the whole media.
    bool pendingAnalysis = false;
    int32_t pendingChapter = -1;

    // The names Rename chapters would give, one per chapter in table order,
    // worked out when the button is pressed and shown for confirmation.
    std::vector<std::string> renamePreview;

    bool focusNext = false;

    void waveformReady(const class WaveformProcessingFinishedEvent* ev) noexcept;
    static void autoChapterNames(const ChapterState& chapterState, std::vector<std::string>& out) noexcept;
    void showRenameControls(ChapterState& chapterState) noexcept;

    bool audioEnvelope(const std::vector<float>*& outSamples, float& outRate) noexcept;
    void showAnalysisControls(ChapterState& chapterState,
        const std::vector<float>* samples, float envelopeRate, bool haveAudio) noexcept;
    void measureChapter(Chapter& chapter,
        const std::vector<float>& samples, float envelopeRate) noexcept;
    void measureAllChapters(ChapterState& chapterState,
        const std::vector<float>& samples, float envelopeRate) noexcept;
    void requestWaveformThen(int32_t chapterIdx) noexcept;
    void createChaptersFromTracks(ChapterState& chapterState,
        const std::vector<float>& samples, float envelopeRate) noexcept;
    void applyTempoToGrid(const OFS_Tempo::TempoResult& tempo) noexcept;
    void applyChapterTempoToGrid(const Chapter& chapter) noexcept;
    static void stampTempoOnChapter(Chapter& chapter, const OFS_Tempo::TempoResult& tempo) noexcept;

    public:
    OFS_ChapterManager() noexcept;
    OFS_ChapterManager(const OFS_ChapterManager&) = delete;
    OFS_ChapterManager(OFS_ChapterManager&&) = delete;
    ~OFS_ChapterManager() noexcept;

    static bool ExportClip(const Chapter& chapter, const std::string& outputDirStr) noexcept;
    void ShowWindow(bool* open) noexcept;

    // For the tempo grid's panel, so detection can be reached from where its
    // result is used. Both fetch the waveform first when there is none.
    // MeasureChapter reads one chapter's tempo; DetectTempo is the Chapters
    // window's main button: lay chapters over the tracks when there are none,
    // or measure every chapter. Both return false when there is no media.
    bool MeasureChapter(Chapter& chapter) noexcept;
    bool DetectTempo() noexcept;
    // Reading the audio or running a detection asked for earlier.
    bool Busy() noexcept;
    // Brings the window to the front the next time it is drawn, for a window
    // docked behind another tab.
    inline void FocusNextFrame() noexcept { focusNext = true; }

    ChapterState& State() noexcept;
};
