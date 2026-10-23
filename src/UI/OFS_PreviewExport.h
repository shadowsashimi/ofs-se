#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A preview video of a script, for sharing: the video with the simulator, a
// strip of the script's tracks scrolling past a playhead, and the heatmap
// drawn over it, as an MP4 with the sound, or an animated WebP or GIF.
//
// ffmpeg does the heavy lifting. It reads the video itself, scales it, lays
// each frame drawn here on top and encodes the result, so the video playing
// in the app is never seeked or paused and the app can be used while a
// preview renders. Frames are drawn off screen a few at a time on the main
// thread, which owns the GL context, and handed to a thread that writes them
// to ffmpeg, so a slow encode never holds up the interface.
class OFS_PreviewExport
{
public:
    enum Format : int32_t { Mp4, Webp, Gif };

    struct Options
    {
        bool video = true;
        bool simulator = true;
        bool timeline = true;
        bool heatmap = true;
        float start = 0.f;
        float end = 30.f;
        int32_t format = Mp4;
        // Height of the output in pixels; the width follows the video.
        int32_t height = 720;
    };

    ~OFS_PreviewExport() noexcept;

    void OpenDialog() noexcept;
    // The dialog and the progress window, every frame.
    void Show() noexcept;
    // Draws and hands on the next frames of a render under way. Called once
    // per app frame, after ImGui's frame has started.
    void Step() noexcept;
    bool Busy() const noexcept { return stage == Stage::Rendering || stage == Stage::Finishing; }
    // Stops a render under way and removes its unfinished file.
    void Cancel() noexcept { if (Busy()) finish(true); }

    // For the UI tests: starts at once, to a path, without the dialog.
    void StartTo(const std::string& path, const Options& options) noexcept;
    const char* StageName() const noexcept;
    int32_t FramesDone() const noexcept { return framesDone; }
    int32_t FramesTotal() const noexcept { return framesTotal; }

private:
    enum class Stage { Idle, Rendering, Finishing, Done, Failed };
    Stage stage = Stage::Idle;
    Options options;
    bool dialogOpen = false;
    std::string outputPath;
    std::string failure;

    int32_t width = 0;
    int32_t height = 0;
    float fps = 30.f;
    int32_t framesTotal = 0;
    int32_t framesDone = 0;

    // Off screen: a framebuffer and its colour texture.
    uint32_t framebuffer = 0;
    uint32_t colorTexture = 0;

    // ffmpeg, and the frames waiting for it.
    struct subprocess_s* process = nullptr;
    std::thread writer;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<std::vector<uint8_t>> queue;
    bool noMoreFrames = false;
    std::atomic<bool> cancelled = false;
    std::atomic<bool> writerDone = false;
    std::atomic<int32_t> exitCode = -1;
    std::string ffmpegErrors;

    void defaultRange() noexcept;
    bool start() noexcept;
    void finish(bool cancel) noexcept;
    void releaseGl() noexcept;
    void drawFrame(struct ImDrawList* drawList, float time, bool first) noexcept;
    void drawTimeline(struct ImDrawList* drawList, float time, float top, float bottom) noexcept;
    void writerLoop() noexcept;
    void showDialog() noexcept;
    void showProgress() noexcept;
};
