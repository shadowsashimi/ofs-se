#include "OFS_VideoplayerWindow.h"

#include "stb_sprintf.h"
#include "stb_image_write.h"
#include "OFS_GL.h"
#include "OFS_Localization.h"

#include "OFS_EventSystem.h"
#include "OFS_ImGui.h"
#include "OFS_Profiling.h"
#include "OFS_Shader.h"

#include "OFS_Videoplayer.h"

#include "state/states/VideoplayerWindowState.h"

bool OFS_VideoplayerWindow::Init(OFS_Videoplayer* player) noexcept
{
	stateHandle = OFS_ProjectState<VideoPlayerWindowState>::Register(VideoPlayerWindowState::StateName);

	this->player = player;
	this->vrShader = std::make_unique<VrShader>();
	
	EV::Queue().appendListener(SDL_MOUSEWHEEL,
		OFS_SDL_Event::HandleEvent(EVENT_SYSTEM_BIND(this, &OFS_VideoplayerWindow::mouseScroll)));
	
	videoImageId = ImGui::GetIDWithSeed("videoImage", 0, rand());
	return true;
}

OFS_VideoplayerWindow::~OFS_VideoplayerWindow() noexcept
{
}

void OFS_VideoplayerWindow::mouseScroll(const OFS_SDL_Event* ev) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	auto& state = VideoPlayerWindowState::State(stateHandle);
	if (state.lockedPosition) return;

	auto& scroll = ev->sdl.wheel;
	if (videoHovered) {
		auto mousePosInVid = ImGui::GetMousePos() - viewportPos - windowPos - state.videoPos;
		float zoomPointX = (mousePosInVid.x - (videoDrawSize.x/2.f)) / videoDrawSize.x;
		float zoomPointY = (mousePosInVid.y - (videoDrawSize.y/2.f)) / videoDrawSize.y;

		float vidWidth = player->VideoWidth();
		float vidHeight = player->VideoHeight();

		switch (state.activeMode) {
		case VideoMode::LeftPane:
		case VideoMode::RightPane:
			vidWidth /= 2.f;
			break;
		case VideoMode::TopPane:
		case VideoMode::BottomPane:
			vidHeight /= 2.f;
			break;
		}
		zoomPointX *= vidWidth;
		zoomPointY *= vidHeight;

		const float oldScale = state.zoomFactor;
		// apply zoom
		if (state.activeMode == VideoMode::VrMode) {
			state.vrZoom *= ((1+(ZoomMulti * scroll.y)));
			state.vrZoom = Util::Clamp(state.vrZoom, 0.05f, 2.0f);
			return;
		}

		state.zoomFactor *= 1 + (ZoomMulti * scroll.y);
		state.zoomFactor = Util::Clamp(state.zoomFactor, 0.0f, 10.f);

		const float scaleChange = (state.zoomFactor - oldScale) * baseScaleFactor;
		const float offsetX = -(zoomPointX * scaleChange);
		const float offsetY = -(zoomPointY * scaleChange);

		state.prevTranslation.x += offsetX;
		state.prevTranslation.y += offsetY;

		if (!dragStarted) {
			state.currentTranslation = state.prevTranslation;
		}
	}
}

void OFS_VideoplayerWindow::drawVrVideo(ImDrawList* draw_list) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	auto& state = VideoPlayerWindowState::State(stateHandle);
	if (!state.lockedPosition && videoHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !dragStarted) {
		dragStarted = true;
	}
	// apply drag to translation
	else if (dragStarted && videoHovered)
	{
		state.currentVrRotation =
			state.prevVrRotation
			+ (ImGui::GetMouseDragDelta(ImGuiMouseButton_Left) 
				/ ImVec2((10000.f * state.vrZoom), (videoDrawSize.y / videoDrawSize.x) * 10000.f * state.vrZoom));
	}

	draw_list->AddCallback(
		[](const ImDrawList* parent_list, const ImDrawCmd* cmd) {
			auto& ctx = *(OFS_VideoplayerWindow*)cmd->UserCallbackData;
			auto& state = VideoPlayerWindowState::State(ctx.stateHandle);

			auto draw_data = OFS_ImGui::CurrentlyRenderedViewport->DrawData;
			ctx.vrShader->Use();

			float L = draw_data->DisplayPos.x;
			float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
			float T = draw_data->DisplayPos.y;
			float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
			const float ortho_projection[4][4] =
			{
				{ 2.0f / (R - L), 0.0f, 0.0f, 0.0f },
				{ 0.0f, 2.0f / (T - B), 0.0f, 0.0f },
				{ 0.0f, 0.0f, -1.0f, 0.0f },
				{ (R + L) / (L - R),  (T + B) / (B - T),  0.0f,   1.0f },
			};
			ctx.vrShader->ProjMtx(&ortho_projection[0][0]);
			ctx.vrShader->Rotation(&state.currentVrRotation.x);
			ctx.vrShader->Zoom(state.vrZoom);
			ctx.vrShader->AspectRatio(ctx.videoDrawSize.x / ctx.videoDrawSize.y);

			if (ctx.player->VideoHeight() > 0) {
				ctx.vrShader->VideoAspectRatio((float)ctx.player->VideoWidth() /(float)ctx.player->VideoHeight());
			}
		}, this);

	OFS::ImageWithId(videoImageId, (void*)(intptr_t)player->FrameTexture(), ImGui::GetContentRegionAvail(), ImVec2(0.f, 0.f), ImVec2(1.f, 1.f));
	videoRightClickMenu();
	videoDrawSize = ImGui::GetItemRectSize();
}

void OFS_VideoplayerWindow::draw2dVideo(ImDrawList* draw_list) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	ImVec2 videoSize(player->VideoWidth(), player->VideoHeight());
	if (videoSize.x <= 0.f || videoSize.y <= 0.f) {
		// Audio, or a video that has not reported its size yet. Scaling by zero
		// would put a NaN rectangle into the draw list.
		return;
	}
	ImVec2 dst = ImGui::GetContentRegionAvail();
	baseScaleFactor = std::min(dst.x / videoSize.x, dst.y / videoSize.y);
	videoSize.x *= baseScaleFactor;
	videoSize.y *= baseScaleFactor;

	ImVec2 uv0(0.f, 0.f);
	ImVec2 uv1(1.f, 1.f);

	auto& state = VideoPlayerWindowState::State(stateHandle);
	switch (state.activeMode) {
		case VideoMode::LeftPane:
			videoSize.x /= 2.f;
			uv1.x = 0.5f;
			break;
		case VideoMode::RightPane:
			videoSize.x /= 2.f;
			uv0.x = 0.5f;
			break;
		case VideoMode::TopPane:
			videoSize.y /= 2.f;
			uv1.y = 0.5;
			break;
		case VideoMode::BottomPane:
			videoSize.y /= 2.f;
			uv0.y = 0.5f;
			break;
		case VideoMode::VrMode:
		case VideoMode::Full:
		default: break;
	}

	videoSize = videoSize * ImVec2(state.zoomFactor, state.zoomFactor);
	state.videoPos = (ImGui::GetWindowSize() - videoSize) * 0.5f + state.currentTranslation;
	ImGui::SetCursorPos(state.videoPos);
	// the videoHovered is one frame old but moving this up prevents flicker while dragging and zooming at the same time
	// start video dragging
	if (!state.lockedPosition && videoHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !dragStarted) {
		dragStarted = true;
	}
	// apply drag to translation
	else if(dragStarted && videoHovered)
	{
		state.currentTranslation = state.prevTranslation + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
	}

	OFS::ImageWithId(videoImageId, (void*)(intptr_t)player->FrameTexture(), videoSize, uv0, uv1);
	videoRightClickMenu();
}

void OFS_VideoplayerWindow::videoRightClickMenu() noexcept
{
	if (ImGui::BeginPopupContextItem())	{
		auto& state = VideoPlayerWindowState::State(stateHandle);
		ImGui::MenuItem(TR(LOCK), NULL, &state.lockedPosition);
		ImGui::EndPopup();
	}
}

// A title and a line of explanation, centred across the panel, with an
// optional button under them. In the middle of the panel by default; at the
// bottom when something else is drawn over the middle.
static void drawCenteredNotice(const char* title, const char* hint,
	const char* buttonLabel, const std::function<void()>& onButton,
	bool atBottom = false) noexcept
{
	const auto& style = ImGui::GetStyle();
	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float maxWrap = ImGui::GetFontSize() * 28.f;
	const float wrap = avail.x * 0.8f < maxWrap ? avail.x * 0.8f : maxWrap;
	const ImVec2 titleSize = ImGui::CalcTextSize(title);
	const ImVec2 hintSize = ImGui::CalcTextSize(hint, nullptr, false, wrap);

	ImVec2 buttonSize(0.f, 0.f);
	if (buttonLabel) {
		buttonSize = ImGui::CalcTextSize(buttonLabel) + (style.FramePadding * 2.f);
	}

	float blockHeight = titleSize.y + style.ItemSpacing.y + hintSize.y;
	if (buttonLabel) blockHeight += style.ItemSpacing.y * 2.f + buttonSize.y;
	const ImVec2 origin = ImGui::GetCursorPos();

	const float top = atBottom
		? origin.y + avail.y - blockHeight - (style.WindowPadding.y * 2.f)
		: origin.y + ((avail.y - blockHeight) * 0.5f);
	ImGui::SetCursorPos(ImVec2(origin.x + ((avail.x - titleSize.x) * 0.5f), top));
	ImGui::TextUnformatted(title);
	ImGui::SetCursorPosX(origin.x + ((avail.x - hintSize.x) * 0.5f));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
	ImGui::TextDisabled("%s", hint);
	ImGui::PopTextWrapPos();

	if (buttonLabel) {
		ImGui::Spacing();
		ImGui::SetCursorPosX(origin.x + ((avail.x - buttonSize.x) * 0.5f));
		if (ImGui::Button(buttonLabel) && onButton) {
			onButton();
		}
	}
}

void OFS_VideoplayerWindow::DrawVideoPlayer(bool* open, bool* drawVideo) noexcept
{
	OFS_PROFILE(__FUNCTION__);
	if (open != nullptr && !*open) return;
	
	ImGui::Begin(TR_ID("VIDEOPLAYER", Tr::VIDEOPLAYER), open, ImGuiWindowFlags_None | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);

	if (!player->VideoLoaded()) {
		// Nothing open yet. This used to be an empty panel, which is the first
		// thing a new user sees and says nothing about what to do.
		drawCenteredNotice("Nothing open",
			"Open a video or audio file from File > Open, or drop one onto this window.",
			OnStartBlankProject ? "Script without a video" : nullptr,
			OnStartBlankProject);
		ImGui::End();
		return;
	}

	if (player->IsBlank()) {
		// A project with no media at all. There is no picture coming, so say
		// so rather than leaving the panel blank. At the bottom, because the
		// simulator sits over the middle of this panel by default and would
		// cover it.
		drawCenteredNotice("No video",
			"This script has no media. Everything else works from the timeline; "
			"attach a video any time from Project > Pick different media.",
			nullptr, nullptr, true);
		ImGui::End();
		return;
	}

	if (player->IsAudioOnly()) {
		// Sound with no picture. mpv plays it without ever drawing a frame, so
		// this panel would otherwise stay black and look broken. At the bottom
		// for the same reason as above.
		auto name = Util::PathFromString(player->VideoPath()).filename().u8string();
		drawCenteredNotice(name.empty() ? "Audio only" : name.c_str(),
			"Audio only, so there is no picture to show. Script it from the "
			"timeline, where the waveform draws the sound.",
			nullptr, nullptr, true);
		ImGui::End();
		return;
	}

	if (*drawVideo) {
		viewportPos = ImGui::GetWindowViewport()->Pos;
		auto drawList = ImGui::GetWindowDrawList();
		auto& state = VideoPlayerWindowState::State(stateHandle);
		if (state.activeMode != VideoMode::VrMode) {
			draw2dVideo(drawList);
		}
		else {
			drawVrVideo(drawList);
		}
		drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
	
		videoHovered = ImGui::IsItemHovered() && ImGui::IsWindowHovered();
		VideoScreenMin = ImGui::GetItemRectMin();
		VideoScreenMax = ImGui::GetItemRectMax();
		VideoScreenFrame = ImGui::GetFrameCount();
		videoDrawSize = ImGui::GetItemRectSize();
	
		// cancel drag
		if ((dragStarted && !videoHovered) || ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
			dragStarted = false;
			state.prevTranslation = state.currentTranslation;
			state.prevVrRotation = state.currentVrRotation;
		}
	
		// recenter
		if (videoHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
			ResetTranslationAndZoom();
		}
	}
	else
	{
		if (ImGui::Button(TR(CLICK_TO_ENABLE_VIDEO))) {
			*drawVideo = true;
		}
	}
	windowPos = ImGui::GetWindowPos() - viewportPos;
	ImGui::End();
}

void OFS_VideoplayerWindow::ResetTranslationAndZoom() noexcept
{
	auto& state = VideoPlayerWindowState::State(stateHandle);
	if (state.lockedPosition) return;
	state.zoomFactor = 1.f;
	state.prevTranslation = ImVec2(0.f, 0.f);
	state.currentTranslation = ImVec2(0.f, 0.f); 
}