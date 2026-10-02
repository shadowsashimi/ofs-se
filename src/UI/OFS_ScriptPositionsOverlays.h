#pragma once
#include "ScriptPositionsOverlayMode.h"
#include "OFS_Localization.h"
#include <cstdint>


// ATTENTION: no reordering
enum ScriptingOverlayModes : int32_t {
	FRAME,
	TEMPO,
	EMPTY,
};


class ScriptTimeline;

// How finely points placed or dragged with the mouse have their position
// rounded, as a dropdown. Shared by the toolbar and Preferences.
void DrawPositionRoundingSelector(const char* id) noexcept;

class TempoOverlay : public BaseOverlay {
public:
	// Beats per grid line, from a whole bar down to a 64th. Public because the
	// note length is a musical unit rather than a detail of drawing the grid:
	// the beat fill tool writes points at these spacings too, and must mean
	// the same thing by "1/8" as the grid does.
	static constexpr std::array<float, 10> beatMultiples{
		4.f * 1.f,
		4.f * (1.f / 2.f),
		4.f * (1.f / 4.f),
		4.f * (1.f / 8.f),
		4.f * (1.f / 12.f),
		4.f * (1.f / 16.f),
		4.f * (1.f / 24.f),
		4.f * (1.f / 32.f),
		4.f * (1.f / 48.f),
		4.f * (1.f / 64.f),
	};
private:
	static constexpr std::array<uint32_t, 10> beatMultipleColor{
		IM_COL32(0xbb, 0xbe, 0xbc, 0xFF), // 1st ???

		IM_COL32(0x53, 0xd3, 0xdf, 0xFF), // 2nds
		IM_COL32(0xc1, 0x65, 0x77, 0xFF), // 4ths
		IM_COL32(0x24, 0x54, 0x99, 0xFF), // 8ths
		IM_COL32(0xc8, 0x86, 0xee, 0xFF), // 12ths
		IM_COL32(0xd2, 0xcc, 0x23, 0xFF), // 16ths
		IM_COL32(0xea, 0x8d, 0xe0, 0xFF), // 24ths
		IM_COL32(0xe7, 0x97, 0x5c, 0xFF), // 32nds
		IM_COL32(0xeb, 0x38, 0x99, 0xFF), // 48ths
		IM_COL32(0x23, 0xd2, 0x54, 0xFF), // 64ths
	};
	static constexpr std::array<Tr, 10> beatMultiplesStrings{
		Tr::TEMPO_WHOLE_MEASURES,
		Tr::TEMPO_2ND_MEASURES,
		Tr::TEMPO_4TH_MEASURES,
		Tr::TEMPO_8TH_MEASURES,
		Tr::TEMPO_12TH_MEASURES,
		Tr::TEMPO_16TH_MEASURES,
		Tr::TEMPO_24TH_MEASURES,
		Tr::TEMPO_32ND_MEASURES,
		Tr::TEMPO_48TH_MEASURES,
		Tr::TEMPO_64TH_MEASURES,
	};
	uint32_t stateHandle = 0xFFFF'FFFF;
	uint32_t chapterStateHandle = 0xFFFF'FFFF;

	// The chapter the playhead is inside, or null when it is in a gap between
	// chapters or the project has none. Shared by everything that has to ask,
	// so the grid, the write back and the greying of the BPM field cannot
	// disagree about which chapter is in charge.
	struct Chapter* chapterUnderPlayhead() noexcept;

	// The chapter actually driving the grid: the one under the playhead, if
	// automatic mode is on and it has a tempo to give. Null means the BPM is
	// the user's to set.
	struct Chapter* tempoDriver(const struct TempoOverlayState& tempo) noexcept;

	// Pulls the tempo of the chapter under the playhead onto the grid. Only
	// does anything while automatic mode is on.
	void followChapterTempo(struct TempoOverlayState& tempo) noexcept;

	// Automatic mode treats the chapter as the source of truth and reapplies it
	// every frame, so a correction made by hand has to reach the chapter or it
	// is gone by the next one.
	void writePhaseToChapter(const struct TempoOverlayState& tempo) noexcept;
public:
	TempoOverlay(ScriptTimeline* timeline) noexcept;
	// How far apart the tempo grid's lines are, as a dropdown of note lengths:
	// 1/4 a line every quarter note, 1/8 every eighth, and so on. Works on the
	// saved setting, so it can be drawn whether or not the tempo grid is the
	// one in use; enabled says whether it should be.
	static void DrawNoteDivisionSelector(const char* id, bool enabled) noexcept;
	virtual void DrawSettings() noexcept override;
	virtual void DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept override;
	virtual void nextFrame(float realFrameTime) noexcept override;
	virtual void previousFrame(float realFrameTime) noexcept override;

	virtual float steppingIntervalForward(float realFrameTime, float fromTime) noexcept override;
	virtual float steppingIntervalBackward(float realFrameTime, float fromTime) noexcept override;

	virtual float SnapTime(float time) noexcept override;
	virtual bool HasSnapGrid() const noexcept override { return true; }
	virtual const char* SnapGridLabel() const noexcept override;
};


class FrameOverlay : public BaseOverlay {
private:
	float fpsOverride = 0.f;
	bool enableFpsOverride = false;
public:
	FrameOverlay(ScriptTimeline* timeline)
		: BaseOverlay(timeline) {}
	virtual void DrawScriptPositionContent(const OverlayDrawingCtx& ctx) noexcept override;
	virtual void DrawSettings() noexcept override;
	virtual void nextFrame(float realFrameTime) noexcept override;
	virtual void previousFrame(float realFrameTime) noexcept override;

	virtual float SnapTime(float time) noexcept override;
	virtual bool HasSnapGrid() const noexcept override { return true; }
	virtual const char* SnapGridLabel() const noexcept override { return "frames"; }

	virtual float logicalFrameTime(float realFrameTime) noexcept override;
	virtual float steppingIntervalForward(float realFrameTime, float fromTime) noexcept override;
	virtual float steppingIntervalBackward(float realFrameTime, float fromTime) noexcept override;
};