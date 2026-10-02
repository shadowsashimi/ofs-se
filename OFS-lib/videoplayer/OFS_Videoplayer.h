#pragma once

#include <cstdint>
#include <string>

#include "OFS_VideoplayerEvents.h"

class OFS_Videoplayer
{
    private:
    // Implementation data
    void* ctx = nullptr;
    // A OpenGL 2D_TEXTURE expected to contain the current video frame.
    uint32_t frameTexture = 0;
    // The position which was last requested via any of the seeking functions.
    float logicalPosition = 0.f;
    // Helper for Mute/Unmute
    float lastVolume = 0.f;
    VideoplayerType playerType;
    
    public:
    OFS_Videoplayer(VideoplayerType playerType) noexcept;
    ~OFS_Videoplayer() noexcept;

	static constexpr float MinPlaybackSpeed = 0.05f;
	static constexpr float MaxPlaybackSpeed = 3.0f;

	// A blank timeline shorter than this leaves no room to script and would
	// divide the position by something close to zero.
	static constexpr float MinBlankDuration = 1.f;
	static constexpr float MaxBlankDuration = 24.f * 60.f * 60.f;

    bool Init(bool hwAccel) noexcept;
    void OpenVideo(const std::string& path) noexcept;

    // Opens a timeline of a fixed length with nothing behind it, for scripting
    // without a video. Everything the rest of the app asks the player for -
    // duration, position, play/pause, speed - is answered from a clock kept
    // here instead of from mpv, so no caller needs to know the difference.
    void OpenBlank(float durationSeconds) noexcept;
    // Changes the length of a blank timeline. Does nothing with media open,
    // which carries its own length.
    void SetBlankDuration(float durationSeconds) noexcept;
    void SetSpeed(float speed) noexcept;
	void AddSpeed(float speed) noexcept;
    void SetVolume(float volume) noexcept;
    
    // All seeking functions must update logicalPosition
    void SetPositionExact(float timeSeconds, bool pausesVideo = false) noexcept;
    void SetPositionPercent(float percentPos, bool pausesVideo = false) noexcept;
    void SeekRelative(float timeSeconds) noexcept;
    void SeekFrames(int32_t offset) noexcept;

    void SetPaused(bool paused) noexcept;
    void TogglePlay() noexcept { SetPaused(!IsPaused()); }
    void CycleSubtitles() noexcept;
    void CloseVideo() noexcept;
    void SaveFrameToImage(const std::string& directory) noexcept;
    void NotifySwap() noexcept;

    inline void Mute() noexcept {
        lastVolume = Volume();
        SetVolume(0.f);
    }
    inline void Unmute() noexcept {
        SetVolume(lastVolume);
    }
    inline void SyncWithPlayerTime() noexcept { SetPositionExact(CurrentPlayerTime()); }
    void Update(float delta) noexcept;

    uint16_t VideoWidth() const noexcept;
    uint16_t VideoHeight() const noexcept;
    float FrameTime() const noexcept;
    float CurrentSpeed() const noexcept;
    float Volume() const noexcept;
    double Duration() const noexcept;
    bool IsPaused() const noexcept;
    float Fps() const noexcept;
    bool VideoLoaded() const noexcept;
    // Whether the open timeline is the blank one rather than a media file.
    bool IsBlank() const noexcept;
    // Whether the open file is sound alone, with no picture and no cover art.
    // Known once the file has loaded, and false until then.
    bool IsAudioOnly() const noexcept;
    // Whether there is a picture to draw: false for a blank timeline and for
    // audio, both of which load and play with nothing on screen.
    bool HasVisual() const noexcept;
    void NextFrame() noexcept;
    void PreviousFrame() noexcept;

    // Uses the logical position which may be different from CurrentPlayerPosition()
    float CurrentPercentPosition() const noexcept;
    // Also uses the logical position
    double CurrentTime() const noexcept;

    // The "actual" position reported by the player
    double CurrentPlayerPosition() const noexcept; 
    double CurrentPlayerTime() const noexcept { return CurrentPlayerPosition() * Duration(); }

    const char* VideoPath() const noexcept;
    inline uint32_t FrameTexture() const noexcept { return frameTexture; }
};