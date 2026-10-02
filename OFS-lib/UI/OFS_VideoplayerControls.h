#pragma once

#include "GradientBar.h"
#include "OFS_Videopreview.h"
#include "FunscriptHeatmap.h"

// Both are structs in ChapterState.h. MSVC mangles an elaborated
// "class Chapter" differently from "struct Chapter", so declaring them one
// way here and the other way where they are defined costs a link error.
struct Chapter;
struct Bookmark;

class OFS_VideoplayerControls
{
private:
	float actualPlaybackSpeed = 1.f;
	float lastPlayerPosition = 0.0f;

	uint32_t chapterStateHandle = 0xFFFF'FFFF;

	uint32_t measureStartTime = 0;
	bool mute = false;
	bool hasSeeked = false;
	bool dragging = false;
	
	static constexpr int32_t PreviewUpdateMs = 1000;
	uint32_t lastPreviewUpdate = 0;
	class OFS_Videoplayer* player = nullptr;

	bool DrawChapter(ImDrawList* drawList, const ImRect& frameBB, Chapter& chapter, ImDrawFlags drawFlags, float currentTime) noexcept;
	bool DrawBookmark(ImDrawList* drawList, const ImRect& frameBB, Bookmark& bookmark) noexcept;
	void DrawChapterWidget(ImDrawList* drawList, float currentTime) noexcept;

	void VideoLoaded(const class VideoLoadedEvent* ev) noexcept;
	bool DrawTimelineWidget(const char* label, float* position) noexcept;
public:
	static constexpr const char* TimeId = "###TIME";

	std::unique_ptr<VideoPreview> videoPreview;
	std::unique_ptr<FunscriptHeatmap> Heatmap;

	void Init(class OFS_Videoplayer* player, bool hwAccel) noexcept;

	inline void UpdateHeatmap(float totalDuration, const FunscriptArray& actions) noexcept
	{
		Heatmap->Update(totalDuration, actions);
	}

	void DrawTimeline() noexcept;

	std::vector<uint8_t> RenderHeatmapToBitmapWithChapters(int16_t width, int16_t height, int16_t chapterHeight) noexcept;
};