#include "OFS_ScriptTimeline.h"

#include <algorithm>
#include "OFS_Profiling.h"
#include "OFS_VideoplayerEvents.h"

#include "stb_sprintf.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include "OFS_ImGui.h"
#include "OFS_SashimiTheme.h"
#include "OFS_Shader.h"
#include "OFS_GL.h"
#include "OFS_EventSystem.h"

#include "state/states/BaseOverlayState.h"
#include "state/states/WaveformState.h"

#include "SDL_events.h"
#include "SDL_timer.h"

// The one place a mouse position becomes an action, for both creating a point
// and dragging one, so it is the only place snapping has to be applied.
//
// Time snapping is asked of the overlay rather than worked out here: the tempo
// grid knows its own division and phase, the frame grid knows the frame time,
// and an overlay without a grid returns the time unchanged. Position snapping
// is not a property of any grid, so it is handled directly.
inline static FunscriptAction getActionForPoint(const OverlayDrawingCtx& ctx, ImVec2 point,
	BaseOverlay* overlay, const BaseOverlayState& state) noexcept
{
	auto localCoord = point - ctx.canvasPos;
	float relativeX = localCoord.x / ctx.canvasSize.x;
	float relativeY = localCoord.y / ctx.canvasSize.y;
	float atTime = ctx.offsetTime + (relativeX * ctx.visibleTime);
	float pos = Util::Clamp<float>(100.f - (relativeY * 100.f), 0.f, 100.f);

	// Alt is the usual hold-to-ignore-the-grid modifier, and Shift is already
	// taken here for add and move.
	if(!ImGui::GetIO().KeyAlt) {
		if(state.SnapToGrid && overlay != nullptr) {
			atTime = overlay->SnapTime(atTime);
		}
		if(state.SnapPositionStep > 0) {
			const float step = (float)state.SnapPositionStep;
			pos = Util::Clamp<float>(std::round(pos / step) * step, 0.f, 100.f);
		}
	}
	return FunscriptAction(atTime, pos);
}

// What a lane is called in its header. Multi-axis scripts are named
// root.axis, and with the root loaded beside them the axis alone tells the
// lanes apart, so that is all that is shown: pitch, roll, twist, with the root
// itself as the stroke. Anything else keeps its full name. The full name is
// always in the tooltip.
static std::string laneLabelFor(const std::vector<std::shared_ptr<Funscript>>& scripts, int idx) noexcept
{
	const std::string& title = scripts[idx]->Title();
	if (title.empty()) return "(untitled)";

	const auto dot = title.rfind('.');
	if (dot != std::string::npos && dot + 1 < title.size()) {
		const std::string root = title.substr(0, dot);
		for (int i = 0; i < (int)scripts.size(); i += 1) {
			if (i != idx && scripts[i]->Title() == root) return title.substr(dot + 1);
		}
	}
	for (int i = 0; i < (int)scripts.size(); i += 1) {
		const std::string& other = scripts[i]->Title();
		if (i != idx && other.size() > title.size() + 1
			&& other.compare(0, title.size(), title) == 0 && other[title.size()] == '.') {
			return "stroke";
		}
	}
	return title;
}

// An eye, for the button that hides a lane. Drawn rather than typed because
// the word "hide" was the same size as the lane's name and got clicked by
// mistake when the name was meant.
static void drawEye(ImDrawList* drawList, ImVec2 center, float size, uint32_t color) noexcept
{
	const float w = size * 0.55f;
	const float h = size * 0.34f;
	constexpr int segments = 10;
	drawList->PathClear();
	for (int i = 0; i <= segments; i += 1) {
		const float t = (float)i / (float)segments;
		drawList->PathLineTo(ImVec2(center.x - w + (2.f * w * t), center.y - (h * sinf(t * IM_PI))));
	}
	for (int i = segments; i >= 0; i -= 1) {
		const float t = (float)i / (float)segments;
		drawList->PathLineTo(ImVec2(center.x - w + (2.f * w * t), center.y + (h * sinf(t * IM_PI))));
	}
	drawList->PathStroke(color, ImDrawFlags_Closed, 1.25f);
	drawList->AddCircleFilled(center, size * 0.15f, color, 12);
}

void ScriptTimeline::updateSelection(const OverlayDrawingCtx& ctx, bool clear) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	float relSel1 = (absSel1 - ctx.offsetTime) / visibleTime;
	float min = std::min(relSel1, relSel2);
	float max = std::max(relSel1, relSel2);

	float startTime = ctx.offsetTime + (visibleTime * min);
	float endTime = ctx.offsetTime + (visibleTime * max);

	float selectionInterval = endTime - startTime;
	// Tiny selections are ignored this is a bit arbitrary.
	// It's supposed to prevent accidentally clearing the selection.
	if(selectionInterval <= 0.008f) // 8ms
		return;
	
	EV::Enqueue<FunscriptShouldSelectTimeEvent>(startTime, endTime, clear, ctx.ActiveScript());
}

int ScriptTimeline::generateWaveformThread(void* userData) noexcept
{
	auto& ctx = *((ScriptTimeline*)userData);
	auto ffmpegPath = Util::FfmpegPath();
	auto outputPath = Util::Prefpath("tmp");
	if (!Util::CreateDirectories(outputPath)) {
		// Still has to report, or anything waiting on the waveform waits for
		// good. The samples stay empty, which is how the waiter knows.
		EV::Enqueue<WaveformProcessingFinishedEvent>();
		return 0;
	}

	outputPath = (Util::PathFromString(outputPath) / "audio.flac").u8string();
	ctx.Wave.data.GenerateAndLoadFlac(ffmpegPath.u8string(), ctx.videoPath, outputPath, ctx.BassWaveform);
	EV::Enqueue<WaveformProcessingFinishedEvent>();
	return 0;
}

void ScriptTimeline::RequestWaveform() noexcept
{
	if(videoPath.empty() || Wave.data.BusyGenerating()) return;
	ShowAudioWaveform = false; // gets switched true after processing

	auto& waveCache = WaveformState::StaticStateSlow();
	auto samples = waveCache.GetSamples();
	// The full mix and the bass alone are two different waveforms of one file,
	// so the cache is keyed by which was asked for as well as by the file.
	if(waveCache.Filename == waveformCacheKey() && !samples.empty())
	{
		Wave.data.SetSamples(std::move(samples));
		// The cache hit needs no ffmpeg, but everything waiting on a waveform
		// waits on this event, so it has to arrive by both routes or the fast
		// one silently never finishes.
		EV::Enqueue<WaveformProcessingFinishedEvent>();
	}
	else
	{
		auto handle = SDL_CreateThread(generateWaveformThread, "OFS_GenWaveform", this);
		SDL_DetachThread(handle);
	}
}

void ScriptTimeline::DrawWaveformSettings() noexcept
{
	const bool busy = Wave.data.BusyGenerating();

	ImGui::BeginDisabled(busy);
	ImGui::Checkbox(TR(ENABLE_WAVEFORM), &ShowAudioWaveform);
	ImGui::Checkbox("Beat ticks", &ShowBeatTicks);
	OFS::Tooltip("Marks along the bottom of the lane where a hit lands, found from the "
				 "same audio as the waveform.");

	{
		static constexpr const char* labels[2] = { "Everything", "Bass only" };
		static constexpr const char* tips[2] = {
			"The whole mix.",
			"Below 150Hz, which is usually the kick and the bass line. Reads the audio again.",
		};
		ImGui::TextDisabled("Waveform of");
		const int32_t picked = OFS::SegmentedControl("##WaveformBand", labels, tips, 2,
			BassWaveform ? 1 : 0);
		if (picked >= 0 && (picked == 1) != BassWaveform) {
			BassWaveform = (picked == 1);
			// The two are different passes of ffmpeg, so the other one has to
			// be made before it can be shown.
			if (!videoPath.empty()) RequestWaveform();
		}
	}
	ImGui::EndDisabled();

	OFS::StepperFloat("Waveform height", "##WaveformScale", &ScaleAudio, 0.25f, 0.01f, 10.f, "%.2fx");
	OFS::Tooltip("Height of the waveform. Raise it for quiet audio.");
	ImGui::ColorEdit3(TR(COLOR), &Wave.WaveformColor.Value.x, ImGuiColorEditFlags_NoInputs);

	if (busy) {
		ImGui::TextDisabled("%s", TR(PROCESSING_AUDIO));
		ImGui::SameLine();
		OFS::Spinner("##WaveformSpin", ImGui::GetFontSize() / 3.f, 4.f, ImGui::GetColorU32(ImGuiCol_TabActive));
	}
	else if (videoPath.empty()) {
		ImGui::TextDisabled("Open a video or audio file to generate its waveform.");
	}
	else if (ImGui::Button(TR(UPDATE_WAVEFORM))) {
		RequestWaveform();
	}
}

void ScriptTimeline::FfmpegAudioProcessingFinished(const WaveformProcessingFinishedEvent* ev) noexcept
{
	ShowAudioWaveform = true;
	beatTicks.clear();
	beatTicksFrom = 0;
	// Update cache
	auto& waveCache = WaveformState::StaticStateSlow();
	waveCache.Filename = waveformCacheKey();
	waveCache.SetSamples(Wave.data.Samples());
	LOG_INFO("Audio processing complete.");
}

void ScriptTimeline::Init()
{
	overlayStateHandle = BaseOverlayState::RegisterStatic();

	EV::Queue().appendListener(SDL_MOUSEWHEEL,
		OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::mouseScroll)));
	EV::Queue().appendListener(WaveformProcessingFinishedEvent::EventType,
		WaveformProcessingFinishedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::FfmpegAudioProcessingFinished)));
	EV::Queue().appendListener(VideoLoadedEvent::EventType,
		VideoLoadedEvent::HandleEvent(EVENT_SYSTEM_BIND(this, &ScriptTimeline::videoLoaded)));

	Wave.Init();
}

void ScriptTimeline::mouseScroll(const OFS_SDL_Event* ev) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	auto& wheel = ev->sdl.wheel;
	constexpr float scrollPercent = 0.10f;
	if (PositionsItemHovered) {
		previousVisibleTime = visibleTime;
		nextVisisbleTime *= 1 + (scrollPercent * -wheel.y);
		nextVisisbleTime = Util::Clamp(nextVisisbleTime, MinVisibleTime, MaxVisibleTime);
		visibleTimeUpdate = SDL_GetTicks();
	}
}

inline static float easeOutExpo(float x) noexcept {
	return x >= 1.f ? 1.f : 1.f - powf(2, -10 * x);
}

void ScriptTimeline::Update() noexcept
{
	auto timePassed = Util::Clamp((SDL_GetTicks() - visibleTimeUpdate) / 150.f, 0.f, 1.f);
	timePassed = easeOutExpo(timePassed);
	visibleTime = Util::Lerp(previousVisibleTime, nextVisisbleTime, timePassed);
}

void ScriptTimeline::videoLoaded(const VideoLoadedEvent* ev) noexcept
{
	if(ev->playerType != VideoplayerType::Main) return;
	videoPath = ev->videoPath;
	auto& waveCache = WaveformState::StaticStateSlow();
	auto samples = waveCache.GetSamples();
	if(waveCache.Filename == waveformCacheKey() && !samples.empty())
	{
		Wave.data.SetSamples(std::move(samples));
		ShowAudioWaveform = true;
		beatTicks.clear();
		beatTicksFrom = 0;
	}
	else 
	{
		ClearAudioWaveform();
	}
}

void ScriptTimeline::handleSelectionScrolling(const OverlayDrawingCtx& ctx) noexcept
{
	constexpr float seekBorderMargin = 0.03f;
	constexpr float scrollSpeed = 80.f;
	if(relSel2 < seekBorderMargin || relSel2 > (1.f - seekBorderMargin)) {
		float seekToTime = ctx.offsetTime + (visibleTime / 2.f);
		seekToTime = Util::Max(0.f, seekToTime);
		float relSeek = (relSel2 < seekBorderMargin) 
			? -(seekBorderMargin - relSel2) 
			: relSel2 - (1.f - seekBorderMargin);

		relSeek *= ImGui::GetIO().DeltaTime * scrollSpeed;

		float seek = visibleTime * relSeek; 
		seekToTime += seek;
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
	}
}

void ScriptTimeline::handleTimelineHover(const OverlayDrawingCtx& ctx) noexcept
{
	if(IsSelecting)
	{
		// Update selection
		relSel2 = (ImGui::GetMousePos().x - ctx.canvasPos.x) / ctx.canvasSize.x;
		relSel2 = Util::Clamp(relSel2, 0.f, 1.f);
	}
	else if(ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		// middle mouse panning
		auto delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
		float timeDelta = (-delta.x / ctx.canvasSize.x) * ctx.visibleTime;
		float seekToTime = (ctx.offsetTime + (ctx.visibleTime/2.f)) + timeDelta;
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
		ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
	}
}

bool ScriptTimeline::handleTimelineClicks(const OverlayDrawingCtx& ctx) noexcept
{
	bool moveOrAddPointModifer = ImGui::IsKeyDown(ImGuiMod_Shift);
	auto mousePos = ImGui::GetMousePos();

	auto leftMouseClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	if(ctx.activeScriptIdx == ctx.drawingScriptIdx && BaseOverlay::PointSize >= 4.f) 
	{
		auto startIt = ctx.DrawingScript()->Actions().begin() + ctx.actionFromIdx;
		auto endIt = ctx.DrawingScript()->Actions().begin() + ctx.actionToIdx;
		for (; startIt != endIt; ++startIt) 
		{
			auto point = BaseOverlay::GetPointForAction(ctx, *startIt);
			const ImVec2 size(BaseOverlay::PointSize, BaseOverlay::PointSize);
			ImRect rect(point - size, point + size);
			bool mouseOnPoint = rect.Contains(mousePos);
			
			if(mouseOnPoint)
			{
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			}

			if (!moveOrAddPointModifer && mouseOnPoint && leftMouseClicked) {
				EV::Enqueue<FunscriptActionClickedEvent>(*startIt, ctx.DrawingScript());
				return true;
			}
			else if(moveOrAddPointModifer && IsMovingIdx < 0 && mouseOnPoint && leftMouseClicked)
			{
				// Start dragging. Grabbing a point that is part of the selection
				// drags the whole selection with it; grabbing any other point
				// makes that point the selection, as it always did.
				if(!ctx.DrawingScript()->IsSelected(*startIt)) {
					ctx.DrawingScript()->ClearSelection();
					ctx.DrawingScript()->SetSelected(*startIt, true);
				}
				IsMovingIdx = ctx.drawingScriptIdx;
				EV::Enqueue<FunscriptActionShouldMoveEvent>(*startIt, ctx.DrawingScript(), true);
				return true;
			}
		}
	}

	if(moveOrAddPointModifer && leftMouseClicked)
	{
		auto newAction = getActionForPoint(ctx, mousePos, activeOverlay, BaseOverlayState::State(overlayStateHandle));
		EV::Enqueue<FunscriptActionShouldCreateEvent>(newAction, ctx.DrawingScript());
		return true;
	}
	else if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		auto mousePos = ImGui::GetMousePos();
		float relX = (mousePos.x - ctx.canvasPos.x) / ctx.canvasSize.x;
		float seekToTime = ctx.offsetTime + (visibleTime * relX);
		EV::Enqueue<ShouldSetTimeEvent>(seekToTime);
		return true;
	}
	else if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Middle))
	{
		ctx.DrawingScript()->ClearSelection();
		return true;
	}
	else if (ctx.hoveredScriptIdx != ctx.activeScriptIdx && leftMouseClicked) {
		EV::Enqueue<ShouldChangeActiveScriptEvent>(ctx.hoveredScriptIdx);
		return true;
	}
	else if(leftMouseClicked)
	{
		// Begin selection
		IsSelecting = true;
		float relSel1 = (mousePos.x - ctx.canvasPos.x) / ctx.canvasSize.x;
		relSel2 = relSel1;
		absSel1 = ctx.offsetTime + (visibleTime * relSel1);
		return true;
	}
	return false;
}

void ScriptTimeline::drawSnapIndicator() noexcept
{
	auto& state = BaseOverlayState::State(overlayStateHandle);
	// Snapping to a grid is only meaningful when the active overlay draws one,
	// so Default mode reports off rather than claiming a snap it will not do.
	const bool gridSnap = state.SnapToGrid && activeOverlay != nullptr && activeOverlay->HasSnapGrid();
	const bool posSnap = state.SnapPositionStep > 0;

	char label[96];
	if(gridSnap && posSnap) {
		stbsp_snprintf(label, sizeof(label), "snap  %s  +  %d", activeOverlay->SnapGridLabel(), state.SnapPositionStep);
	}
	else if(gridSnap) {
		stbsp_snprintf(label, sizeof(label), "snap  %s", activeOverlay->SnapGridLabel());
	}
	else if(posSnap) {
		stbsp_snprintf(label, sizeof(label), "snap  pos %d", state.SnapPositionStep);
	}
	else {
		stbsp_snprintf(label, sizeof(label), "snap  off");
	}

	const ImVec2 pad(6.f, 3.f);
	const ImVec2 textSize = ImGui::CalcTextSize(label);
	const ImVec2 windowPos = ImGui::GetWindowPos();
	const ImVec2 min = windowPos
		+ ImVec2(ImGui::GetWindowSize().x - textSize.x - (pad.x * 2.f) - 12.f, 8.f);
	snapIndicatorRect = ImRect(min, min + textSize + (pad * 2.f));

	// A real item rather than a hand rolled rectangle test. IsWindowHovered is
	// false this late in the frame, once the timeline has submitted its own
	// items, so asking ImGui through an InvisibleButton is both simpler and the
	// only version that actually reports a hover here.
	ImGui::SetCursorScreenPos(snapIndicatorRect.Min);
	ImGui::InvisibleButton("##snapIndicator", snapIndicatorRect.GetSize());
	const bool hovered = ImGui::IsItemHovered();
	if(ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
		state.SnapToGrid = !state.SnapToGrid;
	}

	const bool active = gridSnap || posSnap;
	auto drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(snapIndicatorRect.Min, snapIndicatorRect.Max,
		active ? IM_COL32(0x2E, 0x5D, 0x4E, 0xE0) : IM_COL32(0x26, 0x26, 0x26, 0xB0), 3.f);
	if(hovered) {
		drawList->AddRect(snapIndicatorRect.Min, snapIndicatorRect.Max, IM_COL32(0xD0, 0xD0, 0xD0, 0xFF), 3.f);
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	}
	drawList->AddText(snapIndicatorRect.Min + pad,
		active ? IM_COL32(0xDC, 0xF5, 0xE8, 0xFF) : IM_COL32(0x8A, 0x8A, 0x8A, 0xFF), label);
}

void ScriptTimeline::ShowScriptPositions(
	const OFS_Videoplayer* player,
	BaseOverlay* overlay,
	const std::vector<std::shared_ptr<Funscript>>& scripts,
	int activeScriptIdx) noexcept
{
	OFS_PROFILE(__FUNCTION__);

	auto& style = ImGui::GetStyle();
	OverlayDrawingCtx drawingCtx = {0};
	drawingCtx.offsetTime = player->CurrentTime() - (visibleTime / 2.0);
	drawingCtx.activeScriptIdx = activeScriptIdx;
	drawingCtx.visibleTime = visibleTime;
	drawingCtx.totalDuration = player->Duration();
	drawingCtx.scripts = &scripts;
	drawingCtx.hoveredScriptIdx = -1;
	
	if (drawingCtx.totalDuration == 0.f) return;

	if(IsSelecting) handleSelectionScrolling(drawingCtx);
	
	ImGui::Begin(TR_ID(WindowId, Tr::POSITIONS));
	drawingCtx.drawList = ImGui::GetWindowDrawList();
	PositionsItemHovered = ImGui::IsWindowHovered();
	activeOverlay = overlay;

	// The scripts handle clicks before the indicator is drawn, so a click on it
	// would also drop a point underneath. Suppress that here using the previous
	// frame's rectangle, which is stable because the indicator does not move.
	// Only suppression happens here: the toggle belongs to the button in
	// drawSnapIndicator, or it would fire twice for one click.
	const bool snapIndicatorClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
		&& snapIndicatorRect.Contains(ImGui::GetMousePos());

	drawingCtx.drawnScriptCount = 0;
	for (auto&& script : scripts) {
		if (script->Enabled) { drawingCtx.drawnScriptCount += 1; }
	}

	const auto totalAvailSize = ImGui::GetContentRegionAvail();
	const float verticalSpacingBetweenScripts = style.ItemSpacing.y*2.f;
	auto availSize = totalAvailSize - ImVec2(0.f , verticalSpacingBetweenScripts*((float)drawingCtx.drawnScriptCount-1));
	if (availSize.y < 0.f) { availSize.y = 0.f; }
	const auto startCursor = ImGui::GetCursorScreenPos();
	auto currentCursor = startCursor;

	for(int i=0; i < scripts.size(); i += 1) 
	{
		auto script = scripts[i].get();
		if (!script->Enabled) continue;
		
		drawingCtx.drawingScriptIdx = i;
		drawingCtx.canvasPos = currentCursor;
		drawingCtx.canvasSize = ImVec2(availSize.x, availSize.y / (float)drawingCtx.drawnScriptCount);
		if (Lanes.size() != scripts.size()) Lanes.resize(scripts.size());
		Lanes[i].canvasPos = drawingCtx.canvasPos;
		Lanes[i].canvasSize = drawingCtx.canvasSize;
		Lanes[i].offsetTime = drawingCtx.offsetTime;
		Lanes[i].visibleTime = drawingCtx.visibleTime;

		const auto itemID = ImGui::GetID(script->Title().empty() ? "empty script" : script->Title().c_str());
		ImRect itemBB(drawingCtx.canvasPos, drawingCtx.canvasPos + drawingCtx.canvasSize);
		ImGui::ItemSize(itemBB);
		if (!ImGui::ItemAdd(itemBB, itemID)) {
			continue;
		}

		drawingCtx.drawList->PushClipRect(itemBB.Min - ImVec2(3.f, 3.f), itemBB.Max + ImVec2(3.f, 3.f));

		bool ItemIsHovered = ImGui::IsItemHovered();
		if (ItemIsHovered) {
			drawingCtx.hoveredScriptIdx = i;
		}

		const bool IsActivated = i == activeScriptIdx && drawingCtx.drawnScriptCount > 1;

		// Rounded like every other surface, with a faint light edge along the
		// top so the lane reads as a pane rather than a flat box. Square when
		// the theme's frames are, so turning rounded corners off reaches here.
		const float LaneRounding = ImGui::GetStyle().FrameRounding > 0.f ? 6.f : 0.f;
		const ImVec2 laneMax(drawingCtx.canvasPos.x + drawingCtx.canvasSize.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y);
		drawingCtx.drawList->AddRectFilled(drawingCtx.canvasPos, laneMax,
			IsActivated ? OFS_Sashimi::Role().LaneActiveBg : IM_COL32(0x17, 0x17, 0x17, 255), LaneRounding);
		drawingCtx.drawList->AddRectFilledMultiColor(
			drawingCtx.canvasPos + ImVec2(LaneRounding, 0.f),
			ImVec2(laneMax.x - LaneRounding, drawingCtx.canvasPos.y + (drawingCtx.canvasSize.y * 0.5f)),
			IM_COL32(255, 255, 255, 12), IM_COL32(255, 255, 255, 12),
			IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));

		if (ItemIsHovered) {
			drawingCtx.drawList->AddRectFilled(drawingCtx.canvasPos, laneMax, IM_COL32(255, 255, 255, 8), LaneRounding);
		}

		auto startIt = script->Actions().lower_bound(FunscriptAction(drawingCtx.offsetTime, 0));
		if (startIt != script->Actions().begin()) {
		    startIt -= 1;
		}

		auto endIt = script->Actions().lower_bound(FunscriptAction(drawingCtx.offsetTime + visibleTime, 0));
		if (endIt != script->Actions().end()) {
		    endIt += 1;
		}

		drawingCtx.actionFromIdx = std::distance(script->Actions().begin(), startIt);
		drawingCtx.actionToIdx = std::distance(script->Actions().begin(), endIt);

		if(script->HasSelection())
		{
			auto startIt = script->Selection().lower_bound(FunscriptAction(drawingCtx.offsetTime, 0));
			if (startIt != script->Selection().begin())
				startIt -= 1;

			auto endIt = script->Selection().lower_bound(FunscriptAction(drawingCtx.offsetTime + drawingCtx.visibleTime, 0));
			if (endIt != script->Selection().end())
				endIt += 1;

			drawingCtx.selectionFromIdx = std::distance(script->Selection().begin(), startIt);
			drawingCtx.selectionToIdx = std::distance(script->Selection().begin(), endIt);
		}
		else 
		{
			drawingCtx.selectionFromIdx = 0;
			drawingCtx.selectionToIdx = 0;
		}

		// border
		constexpr float borderThicknes = 1.f;
		uint32_t borderColor = IsActivated ? OFS_Sashimi::Role().Accent : OFS_Sashimi::Grey50;
		if (script->HasSelection()) { 
			borderColor = ImGui::GetColorU32(ImGuiCol_SliderGrabActive); 
		}
		drawingCtx.drawList->AddRect(
			drawingCtx.canvasPos - ImVec2(1, 1),
			laneMax + ImVec2(1, 1),
			borderColor,
			LaneRounding + 1.f, ImDrawFlags_None,
			borderThicknes
		);

		// draws mode specific things in the timeline
		// by default it draws the frame and time dividers
		// DrawAudioWaveform called in scripting mode to control the draw order. spaghetti
		{
			OFS_PROFILE("overlay->DrawScriptPositionContent(drawingCtx)");
			overlay->DrawScriptPositionContent(drawingCtx);
		}

		// current position indicator -> |
		drawingCtx.drawList->AddTriangleFilled(
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f) - ImGui::GetFontSize(), 0.f),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f) + ImGui::GetFontSize(), 0.f),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f), ImGui::GetFontSize()/1.5f),
			IM_COL32(255, 255, 255, 255)
		);
		drawingCtx.drawList->AddLine(
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f)-0.5f, 0),
			drawingCtx.canvasPos + ImVec2((drawingCtx.canvasSize.x/2.f)-0.5f, drawingCtx.canvasSize.y-1.f),
			IM_COL32(255, 255, 255, 255),
		4.0f);

		// selection box
		const auto selectColor = OFS_Sashimi::Role().AccentBright;
		const auto selectColorBackground = (selectColor & ~IM_COL32_A_MASK) | (100u << IM_COL32_A_SHIFT);
		if (IsSelecting && (i == activeScriptIdx)) {
			float relSel1 = (absSel1 - drawingCtx.offsetTime) / visibleTime;
			drawingCtx.drawList->AddRectFilled(drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, 0), drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, drawingCtx.canvasSize.y), selectColorBackground);
			drawingCtx.drawList->AddLine(drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, 0), drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel1, drawingCtx.canvasSize.y), selectColor, 3.0f);
			drawingCtx.drawList->AddLine(drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, 0), drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * relSel2, drawingCtx.canvasSize.y), selectColor, 3.0f);
		}

		// selectionStart currently used for controller select
		if (startSelectionTime >= 0.f) {
			float startSelectRel = (startSelectionTime - drawingCtx.offsetTime) / visibleTime;
			drawingCtx.drawList->AddLine(
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * startSelectRel, 0),
				drawingCtx.canvasPos + ImVec2(drawingCtx.canvasSize.x * startSelectRel, drawingCtx.canvasSize.y),
				selectColor, 3.0f
			);
		}

		// Lane header, over the lane's top left corner: a tick box that makes
		// the lane a target for keyboard edits and the clipboard, an eye that
		// hides the lane, and the lane's name, smaller than body text so it
		// does not sit over the points. Laid out here, ahead of the clicks,
		// because a click on any of it must not also place or select a point
		// underneath; drawn further down, after the context menu, so that menu
		// stays attached to the lane rather than to these.
		const std::string laneTitle = laneLabelFor(scripts, i);
		const float headerFontSize = ImGui::GetFontSize() * 0.85f;
		const float headerBox = std::floor(headerFontSize);
		const ImVec2 headerPad(7.f, 3.f);
		constexpr float headerGap = 7.f;
		const ImVec2 headerOrigin = drawingCtx.canvasPos + ImVec2(8.f, 6.f);
		const float headerHeight = headerBox + (headerPad.y * 2.f);
		const ImVec2 laneTitleSize = ImGui::GetFont()->CalcTextSizeA(headerFontSize, FLT_MAX, 0.f, laneTitle.c_str());
		const ImRect laneTargetRect(headerOrigin + headerPad, headerOrigin + headerPad + ImVec2(headerBox, headerBox));
		const ImRect laneHideRect(ImVec2(laneTargetRect.Max.x + headerGap, laneTargetRect.Min.y),
			ImVec2(laneTargetRect.Max.x + headerGap + (headerBox * 1.35f), laneTargetRect.Max.y));
		const ImRect laneNameRect(ImVec2(laneHideRect.Max.x + headerGap, headerOrigin.y),
			ImVec2(laneHideRect.Max.x + headerGap + laneTitleSize.x + headerPad.x, headerOrigin.y + headerHeight));
		const bool laneCanHide = drawingCtx.drawnScriptCount > 1;
		const ImRect laneLabelBounds(headerOrigin, laneNameRect.Max);
		const bool laneLabelClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
			&& laneLabelBounds.Contains(ImGui::GetMousePos());

		// Handle action clicks
		if(!snapIndicatorClicked && !laneLabelClicked && ItemIsHovered && handleTimelineClicks(drawingCtx)) { /* click was handled */ }
		else if(drawingCtx.drawingScriptIdx == IsMovingIdx)
		{
			if(ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) 
			{
				// Update dragged action
				auto mousePos = ImGui::GetMousePos();
				auto newAction = getActionForPoint(drawingCtx, mousePos, activeOverlay, BaseOverlayState::State(overlayStateHandle));
				EV::Enqueue<FunscriptActionShouldMoveEvent>(newAction, scripts[i], false);
			}
			else
			{
				// Stop dragging
				IsMovingIdx = -1;
			}
		}
		else if(IsSelecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
		{
			IsSelecting = false;
			bool clearSelection = !(SDL_GetModState() & KMOD_CTRL);
			updateSelection(drawingCtx, clearSelection);
		}
		else if(IsMovingIdx < 0 && ItemIsHovered)
		{
			handleTimelineHover(drawingCtx);
		}

		ImVec2 newCursor(drawingCtx.canvasPos.x, drawingCtx.canvasPos.y + drawingCtx.canvasSize.y + verticalSpacingBetweenScripts);
		if (newCursor.y < (startCursor.y + totalAvailSize.y)) { currentCursor = newCursor; }

		// right click context menu
		if (ImGui::BeginPopupContextItem(script->Title().c_str()))
		{
			if (DrawPointActionsMenu) DrawPointActionsMenu();
			if (ImGui::BeginMenu(TR_ID("SCRIPTS", Tr::SCRIPTS))) {
				for (auto& script : scripts) {
					if(script->Title().empty()) {
						ImGui::TextDisabled(TR(NONE));
						continue;
					}
					ImGui::PushItemFlag(ImGuiItemFlags_Disabled, drawingCtx.drawnScriptCount == 1 && script->Enabled);
					ImGui::PushID(script.get());
					if (ImGui::Checkbox("##shown", &script->Enabled) && !script->Enabled) {
						if (i == activeScriptIdx) {
							// find a enabled script which can be set active
							for (int i = 0; i < scripts.size(); i += 1) {
								if (scripts[i]->Enabled) {									
									EV::Enqueue<ShouldChangeActiveScriptEvent>(i);
									break;
								}
							}
						}
					}
					OFS::Tooltip("Shown in the timeline.");
					ImGui::PopItemFlag();
					ImGui::SameLine();
					ImGui::Checkbox(script->Title().c_str(), &script->Targeted);
					OFS::Tooltip("Edited together with the active lane, the same as the tick box in its header.");
					ImGui::PopID();
				}
				ImGui::EndMenu();
			}
			// The two settings reached for while scripting stay one click away.
			// Everything else about how the timeline draws and snaps lives in
			// Preferences, which this menu used to repeat in three submenus.
			ImGui::Separator();
			{
				auto& overlayState = BaseOverlayState::State(overlayStateHandle);
				ImGui::MenuItem(TR(SNAP_TO_GRID), 0, &overlayState.SnapToGrid,
					activeOverlay != nullptr && activeOverlay->HasSnapGrid());
				OFS::Tooltip("Snaps points placed or dragged with the mouse onto the grid of "
							 "the current mode. Hold Alt to place one point off the grid.");
			}
			if (Wave.data.BusyGenerating()) {
				ImGui::MenuItem(TR(PROCESSING_AUDIO), NULL, false, false);
				ImGui::SameLine();
				OFS::Spinner("##AudioSpin", ImGui::GetFontSize() / 3.f, 4.f, ImGui::GetColorU32(ImGuiCol_TabActive));
			}
			else if (Wave.data.SampleCount() > 0) {
				ImGui::MenuItem(TR(ENABLE_WAVEFORM), NULL, &ShowAudioWaveform);
			}
			else if (ImGui::MenuItem("Make waveform", NULL, false, CanGenerateWaveform())) {
				RequestWaveform();
			}
			if (OnOpenSettings) {
				ImGui::Separator();
				if (ImGui::MenuItem("Timeline settings...")) OnOpenSettings();
				OFS::Tooltip("Lines, points, spline, the playhead line, position rounding, and the "
							 "waveform's height and colour.");
			}
			ImGui::EndPopup();
		}

		// Lane header, laid out above the click handling.
		{
			const bool isActiveLane = i == activeScriptIdx;
			const bool isTargeted = isActiveLane || script->Targeted;
			const ImVec2 savedCursor = ImGui::GetCursorScreenPos();
			ImGui::PushID(i);

			ImGui::SetCursorScreenPos(laneTargetRect.Min);
			ImGui::InvisibleButton("##laneTarget", laneTargetRect.GetSize());
			const bool targetHovered = ImGui::IsItemHovered();
			const bool targetTooltip = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !isActiveLane) {
				script->Targeted = !script->Targeted;
			}

			bool hideHovered = false;
			bool hideTooltip = false;
			bool hideClicked = false;
			ImGui::SetCursorScreenPos(laneHideRect.Min);
			ImGui::InvisibleButton("##laneHide", laneHideRect.GetSize());
			hideHovered = ImGui::IsItemHovered();
			hideTooltip = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
			hideClicked = laneCanHide && ImGui::IsItemClicked(ImGuiMouseButton_Left);

			ImGui::SetCursorScreenPos(laneNameRect.Min);
			ImGui::InvisibleButton("##laneName", laneNameRect.GetSize());
			const bool nameHovered = ImGui::IsItemHovered();
			// Tooltips wait the normal hover delay, like every other tooltip.
			const bool nameTooltip = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !isActiveLane) {
				EV::Enqueue<ShouldChangeActiveScriptEvent>(i);
			}

			ImGui::PopID();
			// Back to where the lanes are being laid out, so the next lane's
			// ItemSize carries on from the right place.
			ImGui::SetCursorScreenPos(savedCursor);

			auto drawList = drawingCtx.drawList;
			// One translucent pill behind the whole header.
			const float pillRounding = ImGui::GetStyle().FrameRounding > 0.f ? headerHeight * 0.5f : 0.f;
			drawList->AddRectFilled(laneLabelBounds.Min, laneLabelBounds.Max,
				isActiveLane ? OFS_Sashimi::Role().LaneActivePill : IM_COL32(0x10, 0x10, 0x10, 0xB8), pillRounding);
			drawList->AddRect(laneLabelBounds.Min, laneLabelBounds.Max, IM_COL32(255, 255, 255, 0x14), pillRounding);

			// Target tick box. The active lane is always a target, so its box
			// is always ticked and does nothing when clicked.
			{
				const bool lit = isTargeted;
				const uint32_t boxFill = lit ? (isActiveLane ? OFS_Sashimi::Role().AccentDeep : OFS_Sashimi::Role().Accent)
					: (targetHovered ? IM_COL32(0x3A, 0x3A, 0x3A, 0xFF) : IM_COL32(0x22, 0x22, 0x22, 0xFF));
				const uint32_t boxEdge = lit ? OFS_Sashimi::Role().AccentBright
					: (targetHovered ? OFS_Sashimi::Grey80 : OFS_Sashimi::Grey50);
				drawList->AddRectFilled(laneTargetRect.Min, laneTargetRect.Max, boxFill, 3.f);
				drawList->AddRect(laneTargetRect.Min, laneTargetRect.Max, boxEdge, 3.f);
				if (lit) {
					const float pad = headerBox * 0.2f;
					ImGui::RenderCheckMark(drawList, laneTargetRect.Min + ImVec2(pad, pad),
						IM_COL32(0xFF, 0xF0, 0xF6, 0xFF), headerBox - (pad * 2.f));
				}
				if (targetHovered) {
					if (!isActiveLane) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
					if (targetTooltip) {
						ImGui::SetTooltip("%s", isActiveLane
							? "The active lane is always edited."
							: (script->Targeted
								? "Edited together with the active lane: points from the keyboard, drag selections, cut, copy and paste all act here too. Click to leave it out."
								: "Click to edit this lane together with the active one: points from the keyboard, drag selections, cut, copy and paste will act here too."));
					}
				}
			}

			// Eye: hides the lane. Dimmed when it is the only lane showing.
			{
				const uint32_t eyeColor = !laneCanHide ? IM_COL32(0x4A, 0x4A, 0x4A, 0xFF)
					: (hideHovered ? OFS_Sashimi::Grey95 : OFS_Sashimi::Grey60);
				if (hideHovered && laneCanHide) {
					drawList->AddRectFilled(laneHideRect.Min - ImVec2(2.f, 2.f), laneHideRect.Max + ImVec2(2.f, 2.f),
						IM_COL32(255, 255, 255, 0x18), 4.f);
					ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				}
				drawEye(drawList, laneHideRect.GetCenter(), headerBox, eyeColor);
				if (hideHovered && hideTooltip) {
					ImGui::SetTooltip("%s", laneCanHide
						? "Hide this lane. Bring it back from the right click menu, under Scripts."
						: "The only lane showing, so it stays.");
				}
			}

			// Name. Click makes the lane active.
			drawList->AddText(ImGui::GetFont(), headerFontSize,
				ImVec2(laneNameRect.Min.x, laneNameRect.Min.y + ((headerHeight - laneTitleSize.y) * 0.5f)),
				isActiveLane ? OFS_Sashimi::Role().AccentPale : (nameHovered ? OFS_Sashimi::Grey95 : OFS_Sashimi::Grey80),
				laneTitle.c_str());
			if (nameHovered) {
				if (!isActiveLane) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
				if (nameTooltip) {
					const std::string& fullTitle = script->Title();
					ImGui::SetTooltip("%s\n%s", fullTitle.empty() ? "(untitled)" : fullTitle.c_str(), isActiveLane
						? "The active script. Keyboard edits and the menus act on this one."
						: "Click to make this the active script.");
				}
			}

			if (hideClicked) {
				script->Enabled = false;
				if (isActiveLane) {
					// Same as unticking it under Scripts: the active script has
					// to stay one that is showing.
					for (int other = 0; other < (int)scripts.size(); other += 1) {
						if (other != i && scripts[other]->Enabled) {
							EV::Enqueue<ShouldChangeActiveScriptEvent>(other);
							break;
						}
					}
				}
			}
		}

		drawingCtx.drawList->PopClipRect();
	}

	// Last, so it sits over the scripts rather than under them.
	drawSnapIndicator();
	ImGui::End();
}

constexpr uint32_t HighRangeCol = IM_COL32(0xE3, 0x42, 0x34, 0xff);
constexpr uint32_t MidRangeCol = IM_COL32(0xE8, 0xD7, 0x5A, 0xff);
constexpr uint32_t LowRangeCol = IM_COL32(0xF7, 0x65, 0x38, 0xff); // IM_COL32(0xff, 0xba, 0x08, 0xff);

// Where the hits are, from the envelope the waveform already holds. A hit is a
// rise in loudness that stands out from the rises around it, which is enough
// to mark a kick or a snare without decoding the audio again.
void ScriptTimeline::updateBeatTicks(float totalDuration) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	const auto& samples = Wave.data.Samples();
	if (samples.size() == beatTicksFrom) return;
	beatTicksFrom = samples.size();
	beatTicks.clear();
	if (samples.size() < 8 || totalDuration <= 0.f) return;

	const float rate = (float)samples.size() / totalDuration;
	// How much louder than its neighbours a rise has to be, and how close two
	// hits may be: a drummer playing 32nds at 200 BPM is still 37ms apart.
	constexpr float StandsOut = 1.6f;
	constexpr float ClosestSeconds = 0.06f;
	const int32_t window = std::max(4, (int32_t)(rate * 0.4f));

	std::vector<float> rise;
	rise.resize(samples.size(), 0.f);
	for (size_t i = 1; i < samples.size(); i += 1) {
		rise[i] = std::max(0.f, samples[i] - samples[i - 1]);
	}

	float lastTick = -1.f;
	for (int32_t i = 1; i < (int32_t)rise.size() - 1; i += 1) {
		if (rise[i] <= rise[i - 1] || rise[i] < rise[i + 1]) continue;

		// Measured against the rises either side rather than against one
		// threshold for the whole track, so a quiet passage still has beats
		// and a loud one is not one long tick.
		const int32_t from = std::max(0, i - window);
		const int32_t to = std::min((int32_t)rise.size() - 1, i + window);
		float sum = 0.f;
		for (int32_t j = from; j <= to; j += 1) sum += rise[j];
		const float around = sum / (float)(to - from + 1);
		if (around <= 0.f || rise[i] < around * StandsOut) continue;

		const float at = (float)i / rate;
		if (lastTick >= 0.f && at - lastTick < ClosestSeconds) continue;
		lastTick = at;
		beatTicks.push_back(at);
	}
}

void ScriptTimeline::drawBeatTicks(const OverlayDrawingCtx& ctx) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	if (beatTicks.empty()) return;

	// Along the bottom of the lane, under the waveform, so they mark the time
	// without drawing over the script.
	const float height = std::min(ctx.canvasSize.y * 0.12f, ImGui::GetFontSize() * 0.6f);
	const float bottom = ctx.canvasPos.y + ctx.canvasSize.y;
	const float visibleEnd = ctx.offsetTime + ctx.visibleTime;
	constexpr uint32_t TickColor = IM_COL32(0xBB, 0xBE, 0xBC, 0x99);

	auto first = std::lower_bound(beatTicks.begin(), beatTicks.end(), ctx.offsetTime);
	for (auto it = first; it != beatTicks.end() && *it <= visibleEnd; ++it) {
		const float x = ctx.canvasPos.x
			+ (((*it - ctx.offsetTime) / ctx.visibleTime) * ctx.canvasSize.x);
		ctx.drawList->AddLine(ImVec2(x, bottom - height), ImVec2(x, bottom), TickColor, 1.f);
	}
}

void ScriptTimeline::DrawAudioWaveform(const OverlayDrawingCtx& ctx) noexcept
{
	OFS_PROFILE(__FUNCTION__);

	if (ShowAudioWaveform && Wave.data.SampleCount() > 0 && ctx.totalDuration > 1.f) {
		auto renderWaveform = [](ScriptTimeline* timeline, const OverlayDrawingCtx& ctx) noexcept
		{
			OFS_PROFILE("DrawAudioWaveform::renderWaveform");
			
			timeline->Wave.Update(ctx);
			
			ctx.drawList->AddCallback([](const ImDrawList* parent_list, const ImDrawCmd* cmd) noexcept {
				ScriptTimeline* ctx = (ScriptTimeline*)cmd->UserCallbackData;
				
				glActiveTexture(GL_TEXTURE1);
				glBindTexture(GL_TEXTURE_2D, ctx->Wave.WaveformTex);
				glActiveTexture(GL_TEXTURE0);
				ctx->Wave.WaveShader->Use();
				auto drawData = OFS_ImGui::CurrentlyRenderedViewport->DrawData;
				float L = drawData->DisplayPos.x;
				float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
				float T = drawData->DisplayPos.y;
				float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
				const float orthoProjection[4][4] =
				{
					{ 2.0f / (R - L), 0.0f, 0.0f, 0.0f },
					{ 0.0f, 2.0f / (T - B), 0.0f, 0.0f },
					{ 0.0f, 0.0f, -1.0f, 0.0f },
					{ (R + L) / (L - R),  (T + B) / (B - T),  0.0f,   1.0f },
				};
				ctx->Wave.WaveShader->ProjMtx(&orthoProjection[0][0]);
				ctx->Wave.WaveShader->AudioData(1);
				ctx->Wave.WaveShader->SampleOffset(ctx->Wave.samplingOffset);
				ctx->Wave.WaveShader->SampleScale(ctx->Wave.samplingScale);
				ctx->Wave.WaveShader->ScaleFactor(ctx->ScaleAudio);
				ctx->Wave.WaveShader->Color(&ctx->Wave.WaveformColor.Value.x);
			}, timeline);

			ctx.drawList->AddImage(0, ctx.canvasPos, ctx.canvasPos + ctx.canvasSize);
			ctx.drawList->AddCallback(ImDrawCallback_ResetRenderState, 0);
		};

		renderWaveform(this, ctx);

		if (ShowBeatTicks) {
			updateBeatTicks(ctx.totalDuration);
			drawBeatTicks(ctx);
		}
	}
}