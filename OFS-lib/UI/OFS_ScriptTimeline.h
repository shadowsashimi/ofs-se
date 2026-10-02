#pragma once
#include "imgui.h"

#include <functional>
#include <vector>
#include <memory>
#include <string>

#include "OFS_Waveform.h"
#include "OFS_Shader.h"
#include "ScriptPositionsOverlayMode.h"
#include "OFS_Videoplayer.h"

#include "OFS_Event.h"
#include "OFS_ScriptTimelineEvents.h"

class ScriptTimeline
{
public:
	uint32_t overlayStateHandle = 0xFFFF'FFFF;
	float absSel1 = 0.f; // absolute selection start
	float relSel2 = 0.f; // relative selection end

	bool IsSelecting = false;
	bool PositionsItemHovered = false;
	int32_t IsMovingIdx = -1;
private:
	void mouseScroll(const OFS_SDL_Event* ev) noexcept;
	void videoLoaded(const class VideoLoadedEvent* ev) noexcept;

	void handleSelectionScrolling(const OverlayDrawingCtx& ctx) noexcept;
	void handleTimelineHover(const OverlayDrawingCtx& ctx) noexcept;
	bool handleTimelineClicks(const OverlayDrawingCtx& ctx) noexcept;

	void updateSelection(const OverlayDrawingCtx& ctx, bool clear) noexcept;

	// Snap state has to be visible to be trusted: it is easy to leave on and
	// then wonder why a point will not go where it is put. Drawn last so it
	// sits over the scripts, and clicked using the previous frame's rectangle,
	// which is stable because the indicator does not move.
	void drawSnapIndicator() noexcept;
	// Runs on its own thread, so it is a member only to reach videoPath and the
	// waveform without handing them over one at a time.
	static int generateWaveformThread(void* userData) noexcept;
	ImRect snapIndicatorRect = ImRect(0.f, 0.f, 0.f, 0.f);
	BaseOverlay* activeOverlay = nullptr;
	void FfmpegAudioProcessingFinished(const WaveformProcessingFinishedEvent* ev) noexcept;

	std::string videoPath;
	uint32_t visibleTimeUpdate = 0;
	float nextVisisbleTime = 5.f;
	float previousVisibleTime = 5.f;

	float visibleTime = 5.f;
	float startSelectionTime = -1.f;
	
	bool ShowAudioWaveform = false;
	// Only the low end of the audio, where the beat usually is. Needs its own
	// pass of ffmpeg, so changing it regenerates the waveform.
	bool BassWaveform = false;
	// Marks under the waveform where a hit lands, found from the same envelope.
	bool ShowBeatTicks = true;
	std::vector<float> beatTicks;
	// The sample count the ticks were found from, so they are found again when
	// a new waveform arrives and not on every frame.
	size_t beatTicksFrom = 0;
	float ScaleAudio = 1.f;

	// What the cached waveform belongs to: the file, and which of its two
	// waveforms it is.
	inline std::string waveformCacheKey() const noexcept
	{
		return BassWaveform ? videoPath + "|bass" : videoPath;
	}

	void updateBeatTicks(float totalDuration) noexcept;
	void drawBeatTicks(const struct OverlayDrawingCtx& ctx) noexcept;
public:
	OFS_WaveformLOD Wave;
	static constexpr const char* WindowId = "###POSITIONS";

	// Drawn at the top of a lane's right click menu, above the view settings.
	// The edits it offers belong to the app, which owns undo and the selection
	// commands, so the timeline only provides the place for them.
	std::function<void()> DrawPointActionsMenu;
	// Opens wherever the app keeps the timeline's settings, from the bottom
	// of the same menu.
	std::function<void()> OnOpenSettings;

	// Where each script's lane was drawn last frame, and the span of time it
	// showed, indexed like the scripts. The UI test driver aims at points and
	// times with it rather than guessing pixels.
	struct LaneGeometry
	{
		ImVec2 canvasPos = ImVec2(0.f, 0.f);
		ImVec2 canvasSize = ImVec2(0.f, 0.f);
		float offsetTime = 0.f;
		float visibleTime = 0.f;
	};
	std::vector<LaneGeometry> Lanes;

	static constexpr float MaxVisibleTime = 300.f;
	static constexpr float MinVisibleTime = 1.f;

	void Init();

	// Generates the audio waveform, or restores it from the cache when this
	// media has been through here before. Public because the waveform is not
	// only something to look at: tempo detection reads the same envelope, and
	// should not have to send the user to a menu in another window to make one
	// before it can answer.
	void RequestWaveform() noexcept;
	inline bool WaveformBusy() noexcept { return Wave.data.BusyGenerating(); }
	inline bool CanGenerateWaveform() noexcept { return !videoPath.empty() && !Wave.data.BusyGenerating(); }
	// The media the timeline last heard was opened; what a waveform would be made from.
	inline const std::string& MediaPath() const noexcept { return videoPath; }
	inline bool WaveformShown() const noexcept { return ShowAudioWaveform; }
	inline bool WaveformIsBass() const noexcept { return BassWaveform; }
	inline int32_t BeatTickCount() const noexcept { return (int32_t)beatTicks.size(); }

	inline void ClearAudioWaveform() noexcept
	{
		ShowAudioWaveform = false;
		Wave.data.Clear();
		beatTicks.clear();
		beatTicksFrom = 0;
	}

	// Show, scale, colour and regenerate, for the Preferences window. The
	// right click menu keeps its own compact copy of the same settings.
	void DrawWaveformSettings() noexcept;
	inline void setStartSelection(float time) noexcept { startSelectionTime = time; }
	inline float selectionStart() const noexcept { return startSelectionTime; }
	void ShowScriptPositions(const OFS_Videoplayer* player, BaseOverlay* overlay, const std::vector<std::shared_ptr<Funscript>>& scripts, int activeScriptIdx) noexcept;

	void Update() noexcept;

	void DrawAudioWaveform(const OverlayDrawingCtx& ctx) noexcept;
};