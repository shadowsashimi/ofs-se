#include "OFS_PreviewExport.h"

#include "OpenFunscripter.h"
#include "OFS_Util.h"
#include "OFS_GL.h"
#include "OFS_ImGui.h"
#include "OFS_SashimiTheme.h"
#include "FunscriptHeatmap.h"
#include "ScriptPositionsOverlayMode.h"
#include "state/states/ChapterState.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#include "subprocess.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr const char* DialogId = "Export preview video###PREVIEW_EXPORT";
constexpr const char* ProgressId = "Preview video###PREVIEW_EXPORT_PROGRESS";
constexpr const char* FormatNames[3] = { "MP4", "WebP", "GIF" };
constexpr const char* FormatExtensions[3] = { ".mp4", ".webp", ".gif" };
constexpr int32_t Heights[3] = { 480, 720, 1080 };
// Frames waiting for ffmpeg. Enough to ride out a slow moment in the encoder
// without holding many seconds of picture in memory.
constexpr size_t QueueLimit = 6;
// The script shown either side of the playhead in the timeline strip.
constexpr float StripSpan = 8.f;
constexpr int32_t StripMaxLanes = 4;

bool mediaHasPicture() noexcept
{
    auto& player = *OpenFunscripter::ptr->player;
    return player.HasVisual() && player.VideoWidth() > 0 && player.VideoHeight() > 0;
}

bool mediaHasSound() noexcept
{
    auto& player = *OpenFunscripter::ptr->player;
    return !player.IsBlank();
}

std::string formatSeconds(float seconds) noexcept
{
    char buf[32];
    Util::FormatTime(buf, sizeof(buf), seconds, true);
    return buf;
}
}

OFS_PreviewExport::~OFS_PreviewExport() noexcept
{
    if (Busy()) finish(true);
}

const char* OFS_PreviewExport::StageName() const noexcept
{
    switch (stage) {
        case Stage::Idle: return "idle";
        case Stage::Rendering: return "rendering";
        case Stage::Finishing: return "finishing";
        case Stage::Done: return "done";
        case Stage::Failed: return "failed";
    }
    return "";
}

void OFS_PreviewExport::defaultRange() noexcept
{
    auto app = OpenFunscripter::ptr;
    const float duration = (float)app->player->Duration();
    const float now = (float)app->player->CurrentTime();
    // The selection, if there is one; else the chapter under the playhead;
    // else half a minute from the playhead, the usual length for a post.
    auto active = app->ActiveFunscript();
    if (active && active->SelectionSize() >= 2) {
        options.start = active->Selection().front().atS;
        options.end = active->Selection().back().atS;
        return;
    }
    const auto& chapters = app->chapterMgr->State().chapters;
    for (const auto& chapter : chapters) {
        if (now >= chapter.startTime && now <= chapter.endTime && chapter.endTime > chapter.startTime) {
            options.start = chapter.startTime;
            options.end = chapter.endTime;
            return;
        }
    }
    options.start = now;
    options.end = Util::Min(duration, now + 30.f);
}

void OFS_PreviewExport::OpenDialog() noexcept
{
    if (Busy()) return;
    defaultRange();
    if (!mediaHasPicture()) options.video = false;
    dialogOpen = true;
}

void OFS_PreviewExport::StartTo(const std::string& path, const Options& wanted) noexcept
{
    if (Busy()) return;
    options = wanted;
    outputPath = path;
    if (!start()) stage = Stage::Failed;
}

bool OFS_PreviewExport::start() noexcept
{
    auto app = OpenFunscripter::ptr;
    failure.clear();
    ffmpegErrors.clear();
    const float duration = (float)app->player->Duration();
    options.start = Util::Clamp(options.start, 0.f, duration);
    options.end = Util::Clamp(options.end, options.start, duration);
    const float length = options.end - options.start;
    if (length < 0.1f) {
        failure = "The range is empty.";
        return false;
    }
    const bool picture = options.video && mediaHasPicture();

    // Even sizes, which H.264 needs, the width following the video's shape.
    height = Util::Clamp(options.height, 120, 2160) & ~1;
    const float aspect = mediaHasPicture()
        ? (float)app->player->VideoWidth() / (float)app->player->VideoHeight()
        : 16.f / 9.f;
    width = ((int32_t)std::round((float)height * aspect)) & ~1;
    width = Util::Clamp(width, 64, 3840);
    // Animated images play at a lower rate, which keeps them small.
    fps = options.format == Mp4 ? 30.f : 20.f;
    framesTotal = Util::Max(1, (int32_t)std::ceil(length * fps));
    framesDone = 0;

    // The off screen picture the overlay is drawn into.
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(1, &colorTexture);
    glBindTexture(GL_TEXTURE_2D, colorTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) {
        releaseGl();
        failure = "Could not set up drawing off screen.";
        return false;
    }

    // ffmpeg: the background first (the video, or a plain dark one), then the
    // frames drawn here from its input, then the media again for the sound
    // when the background is not the video.
    const std::string ffmpeg = Util::FfmpegPath().u8string();
    const std::string media = app->player->VideoPath() ? app->player->VideoPath() : "";
    char startText[32], lengthText[32], sizeText[32], rateText[16];
    snprintf(startText, sizeof(startText), "%.3f", options.start);
    snprintf(lengthText, sizeof(lengthText), "%.3f", length);
    snprintf(sizeText, sizeof(sizeText), "%dx%d", width, height);
    snprintf(rateText, sizeof(rateText), "%d", (int32_t)fps);

    std::vector<std::string> args = { ffmpeg, "-y", "-loglevel", "error", "-nostats" };
    int32_t soundInput = -1;
    if (picture) {
        args.insert(args.end(), { "-ss", startText, "-t", lengthText, "-i", media });
        soundInput = 0;
    }
    else {
        args.insert(args.end(), { "-f", "lavfi", "-t", lengthText, "-i",
            std::string("color=c=0x141414:s=") + sizeText + ":r=" + rateText });
    }
    args.insert(args.end(), { "-f", "rawvideo", "-pix_fmt", "rgba", "-s", sizeText, "-framerate", rateText, "-i", "-" });
    const bool wantSound = options.format == Mp4 && mediaHasSound() && !media.empty();
    if (wantSound && !picture) {
        args.insert(args.end(), { "-ss", startText, "-t", lengthText, "-i", media });
        soundInput = 2;
    }

    // The overlay is drawn upside down, as GL reads it back, with its colours
    // already multiplied by their alpha, as ImGui blends.
    std::string filter = std::string("[0:v]scale=") + sizeText
        + ":force_original_aspect_ratio=decrease,pad=" + std::to_string(width) + ":" + std::to_string(height)
        + ":(ow-iw)/2:(oh-ih)/2,setsar=1,fps=" + rateText + "[bg];"
        + "[1:v]vflip[fg];[bg][fg]overlay=0:0:alpha=premultiplied:shortest=1";
    if (options.format == Gif) {
        filter += ",split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4[v]";
    }
    else {
        filter += "[v]";
    }
    args.insert(args.end(), { "-filter_complex", filter, "-map", "[v]" });
    if (wantSound && soundInput >= 0) {
        args.insert(args.end(), { "-map", std::to_string(soundInput) + ":a?", "-c:a", "aac", "-b:a", "160k" });
    }
    switch (options.format) {
        case Mp4:
            args.insert(args.end(), { "-c:v", "libx264", "-preset", "veryfast", "-crf", "20",
                "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-shortest" });
            break;
        case Webp:
            args.insert(args.end(), { "-c:v", "libwebp_anim", "-quality", "75", "-loop", "0" });
            break;
        case Gif:
            args.insert(args.end(), { "-loop", "0" });
            break;
    }
    args.push_back(outputPath);

    std::vector<const char*> argv;
    for (auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    process = new subprocess_s();
    if (subprocess_create(argv.data(), subprocess_option_no_window, process) != 0) {
        delete process;
        process = nullptr;
        releaseGl();
        failure = "Could not start ffmpeg.";
        return false;
    }

    {
        std::lock_guard lock(queueMutex);
        queue.clear();
        noMoreFrames = false;
    }
    cancelled = false;
    writerDone = false;
    exitCode = -1;
    writer = std::thread([this]() { writerLoop(); });
    stage = Stage::Rendering;
    LOGF_INFO("Preview export started: %s", outputPath.c_str());
    return true;
}

void OFS_PreviewExport::writerLoop() noexcept
{
    FILE* input = subprocess_stdin(process);
    while (true) {
        std::vector<uint8_t> frame;
        {
            std::unique_lock lock(queueMutex);
            queueChanged.wait(lock, [this]() { return !queue.empty() || noMoreFrames || cancelled; });
            if (cancelled) break;
            if (queue.empty() && noMoreFrames) break;
            frame = std::move(queue.front());
            queue.pop_front();
        }
        queueChanged.notify_all();
        if (fwrite(frame.data(), 1, frame.size(), input) != frame.size()) {
            // ffmpeg went away; what it said is read below.
            break;
        }
    }
    // Closing its input tells ffmpeg the frames are done.
    if (process->stdin_file) {
        fclose(process->stdin_file);
        process->stdin_file = nullptr;
    }
    if (cancelled) subprocess_terminate(process);
    int code = -1;
    subprocess_join(process, &code);
    if (FILE* errors = subprocess_stderr(process)) {
        char buf[512];
        size_t got = 0;
        std::string text;
        while ((got = fread(buf, 1, sizeof(buf), errors)) > 0 && text.size() < 4096) text.append(buf, got);
        std::lock_guard lock(queueMutex);
        ffmpegErrors = text;
    }
    exitCode = code;
    writerDone = true;
    queueChanged.notify_all();
}

void OFS_PreviewExport::Step() noexcept
{
    if (stage == Stage::Finishing) {
        if (writerDone) finish(false);
        return;
    }
    if (stage != Stage::Rendering) return;
    if (writerDone) {
        // ffmpeg stopped before it had every frame.
        finish(false);
        return;
    }

    // A few frames each app frame, within a small budget, so the app stays
    // responsive; none while ffmpeg still has a backlog.
    const uint64_t began = SDL_GetPerformanceCounter();
    const float budget = 0.012f;
    while (framesDone < framesTotal) {
        {
            std::lock_guard lock(queueMutex);
            if (queue.size() >= QueueLimit) break;
        }
        const float time = options.start + ((float)framesDone / fps);

        ImDrawList drawList(ImGui::GetDrawListSharedData());
        drawList._ResetForNewFrame();
        drawList.PushTextureID(ImGui::GetIO().Fonts->TexID);
        drawList.PushClipRect(ImVec2(0.f, 0.f), ImVec2((float)width, (float)height));
        drawFrame(&drawList, time, framesDone == 0);
        drawList._PopUnusedDrawCmd();

        ImDrawList* lists[1] = { &drawList };
        ImDrawData data;
        data.Valid = true;
        data.CmdLists = lists;
        data.CmdListsCount = 1;
        data.TotalVtxCount = drawList.VtxBuffer.Size;
        data.TotalIdxCount = drawList.IdxBuffer.Size;
        data.DisplayPos = ImVec2(0.f, 0.f);
        data.DisplaySize = ImVec2((float)width, (float)height);
        data.FramebufferScale = ImVec2(1.f, 1.f);
        ImGuiViewport viewport;
        viewport.DrawData = &data;

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, width, height);
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);
        auto* previous = OFS_ImGui::CurrentlyRenderedViewport;
        OFS_ImGui::CurrentlyRenderedViewport = &viewport;
        ImGui_ImplOpenGL3_RenderDrawData(&data);
        OFS_ImGui::CurrentlyRenderedViewport = previous;
        std::vector<uint8_t> pixels((size_t)width * (size_t)height * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        {
            std::lock_guard lock(queueMutex);
            queue.push_back(std::move(pixels));
        }
        queueChanged.notify_all();
        framesDone += 1;

        const float spent = (float)(SDL_GetPerformanceCounter() - began) / (float)SDL_GetPerformanceFrequency();
        if (spent > budget) break;
    }
    if (framesDone >= framesTotal) {
        {
            std::lock_guard lock(queueMutex);
            noMoreFrames = true;
        }
        queueChanged.notify_all();
        stage = Stage::Finishing;
    }
}

void OFS_PreviewExport::finish(bool cancel) noexcept
{
    if (cancel) {
        cancelled = true;
        queueChanged.notify_all();
    }
    if (writer.joinable()) writer.join();
    if (process) {
        subprocess_destroy(process);
        delete process;
        process = nullptr;
    }
    releaseGl();
    if (cancel) {
        std::error_code ec;
        std::filesystem::remove(Util::PathFromString(outputPath), ec);
        stage = Stage::Idle;
        LOG_INFO("Preview export cancelled.");
        return;
    }
    const bool complete = framesDone >= framesTotal && exitCode == 0;
    if (complete) {
        stage = Stage::Done;
        LOGF_INFO("Preview export saved: %s", outputPath.c_str());
    }
    else {
        stage = Stage::Failed;
        failure = "ffmpeg could not make the video.";
        std::lock_guard lock(queueMutex);
        if (!ffmpegErrors.empty()) {
            failure += "\n" + ffmpegErrors;
            LOGF_ERROR("Preview export failed: %s", ffmpegErrors.c_str());
        }
    }
}

void OFS_PreviewExport::releaseGl() noexcept
{
    if (colorTexture) glDeleteTextures(1, &colorTexture);
    if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
    colorTexture = 0;
    framebuffer = 0;
}

// ---- drawing ---------------------------------------------------------------

void OFS_PreviewExport::drawFrame(ImDrawList* drawList, float time, bool first) noexcept
{
    auto app = OpenFunscripter::ptr;
    const float w = (float)width, h = (float)height;
    const float margin = h * 0.025f;

    // From the bottom up: the heatmap along the very bottom, the strip of
    // tracks over it, and the simulator in what is left.
    float bottom = h - margin;
    if (options.heatmap) {
        const float barH = Util::Max(6.f, h * 0.018f);
        const ImVec2 min(margin, bottom - barH);
        const ImVec2 max(w - margin, bottom);
        app->playerControls.Heatmap->DrawHeatmap(drawList, min, max);
        // Where in the whole video this moment is.
        const float duration = Util::Max(0.001f, (float)app->player->Duration());
        const float x = min.x + ((max.x - min.x) * Util::Clamp(time / duration, 0.f, 1.f));
        drawList->AddRectFilled(ImVec2(x - 1.5f, min.y - 3.f), ImVec2(x + 1.5f, max.y + 3.f), IM_COL32_WHITE);
        bottom = min.y - (margin * 0.5f);
    }
    if (options.timeline) {
        int32_t lanes = 0;
        for (auto& script : app->LoadedFunscripts()) lanes += script->Enabled ? 1 : 0;
        lanes = Util::Clamp(lanes, 1, StripMaxLanes);
        const float laneH = h * 0.055f;
        const float top = bottom - (laneH * (float)lanes);
        drawTimeline(drawList, time, top, bottom);
        bottom = top - (margin * 0.5f);
    }
    if (options.simulator) {
        const ImVec2 min(margin, margin);
        const ImVec2 max(w - margin, bottom);
        app->simulator.DrawExportFrame(drawList, min, max, time, 1.f / fps, first);
    }
}

void OFS_PreviewExport::drawTimeline(ImDrawList* drawList, float time, float top, float bottom) noexcept
{
    auto app = OpenFunscripter::ptr;
    const auto& role = OFS_Sashimi::Role();
    const float w = (float)width, h = (float)height;
    const float margin = h * 0.025f;
    const float left = margin, right = w - margin;
    const float round = h * 0.008f;
    drawList->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), IM_COL32(0x10, 0x10, 0x10, 0xC8), round);

    std::vector<std::shared_ptr<Funscript>> shown;
    for (auto& script : app->LoadedFunscripts()) {
        if (script->Enabled && (int32_t)shown.size() < StripMaxLanes) shown.push_back(script);
    }
    if (shown.empty()) return;
    const float laneH = (bottom - top) / (float)shown.size();
    const float from = time - (StripSpan * 0.5f);
    auto xAt = [&](float t) noexcept { return left + (((t - from) / StripSpan) * (right - left)); };

    ImFont* font = ImGui::GetFont();
    const float fontSize = Util::Max(11.f, h * 0.022f);
    for (size_t lane = 0; lane < shown.size(); lane += 1) {
        const auto& script = shown[lane];
        const float laneTop = top + (laneH * (float)lane);
        const float pad = laneH * 0.16f;
        const float y0 = laneTop + pad, y1 = laneTop + laneH - pad;
        auto yAt = [&](int32_t pos) noexcept { return y1 - ((y1 - y0) * ((float)pos / 100.f)); };
        if (lane > 0) {
            drawList->AddLine(ImVec2(left, laneTop), ImVec2(right, laneTop), IM_COL32(255, 255, 255, 0x18), 1.f);
        }

        // The stretch in view, and a point either side so lines run off the
        // edges rather than stopping short of them.
        const auto& actions = script->Actions();
        auto it = std::lower_bound(actions.begin(), actions.end(), from,
            [](const FunscriptAction& a, float t) { return a.atS < t; });
        if (it != actions.begin()) --it;
        const FunscriptAction* prev = nullptr;
        drawList->PushClipRect(ImVec2(left, laneTop), ImVec2(right, laneTop + laneH), true);
        for (; it != actions.end(); ++it) {
            const FunscriptAction& action = *it;
            const ImVec2 p(xAt(action.atS), yAt(action.pos));
            if (prev) {
                const ImVec2 q(xAt(prev->atS), yAt(prev->pos));
                // In the speed colours picked for the timeline.
                const float span = Util::Max(1e-4f, action.atS - prev->atS);
                const float speed = std::abs((float)(action.pos - prev->pos)) / span;
                const ImColor color = BaseOverlay::SpeedColor(speed);
                drawList->AddLine(q, p, IM_COL32(0, 0, 0, 0xA0), Util::Max(3.f, h * 0.006f));
                drawList->AddLine(q, p, color, Util::Max(1.5f, h * 0.003f));
            }
            drawList->AddCircleFilled(p, Util::Max(2.f, h * 0.0045f), OFS_Sashimi::Grey95);
            prev = &action;
            if (action.atS > from + StripSpan) break;
        }
        drawList->PopClipRect();
        drawList->AddText(font, fontSize, ImVec2(left + (h * 0.01f), laneTop + (h * 0.004f)),
            role.TextFaint, script->Title().c_str());
    }

    // The playhead, in the middle.
    const float mid = (left + right) * 0.5f;
    drawList->AddLine(ImVec2(mid, top), ImVec2(mid, bottom), IM_COL32_WHITE, Util::Max(1.5f, h * 0.003f));
}

// ---- interface -------------------------------------------------------------

void OFS_PreviewExport::Show() noexcept
{
    showDialog();
    showProgress();
}

void OFS_PreviewExport::showDialog() noexcept
{
    if (dialogOpen && !ImGui::IsPopupOpen(DialogId)) ImGui::OpenPopup(DialogId);
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(main->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(DialogId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    auto app = OpenFunscripter::ptr;
    const float em = ImGui::GetFontSize();

    ImGui::TextDisabled("Show");
    const bool picture = mediaHasPicture();
    ImGui::BeginDisabled(!picture);
    ImGui::Checkbox("Video", &options.video);
    ImGui::EndDisabled();
    if (!picture) OFS::Tooltip("This script has no picture to show; the preview has a plain background.");
    ImGui::SameLine();
    ImGui::Checkbox("Simulator", &options.simulator);
    OFS::Tooltip("The simulator as you have it set up: the 3D model, or the bar in 2D.");
    ImGui::SameLine();
    ImGui::Checkbox("Timeline", &options.timeline);
    OFS::Tooltip("A strip of the showing tracks, up to four, scrolling past a playhead.");
    ImGui::SameLine();
    ImGui::Checkbox("Heatmap", &options.heatmap);
    OFS::Tooltip("The whole script's heatmap along the bottom, with a mark at the moment shown.");

    ImGui::Spacing();
    ImGui::TextDisabled("Range");
    ImGui::SetNextItemWidth(em * 7.f);
    ImGui::InputFloat("Start (s)", &options.start, 1.f, 10.f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.f);
    ImGui::InputFloat("End (s)", &options.end, 1.f, 10.f, "%.2f");
    if (ImGui::SmallButton("Selection")) {
        auto active = app->ActiveFunscript();
        if (active && active->SelectionSize() >= 2) {
            options.start = active->Selection().front().atS;
            options.end = active->Selection().back().atS;
        }
    }
    OFS::Tooltip("From the first to the last selected point.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Chapter")) {
        const float now = (float)app->player->CurrentTime();
        for (const auto& chapter : app->chapterMgr->State().chapters) {
            if (now >= chapter.startTime && now <= chapter.endTime) {
                options.start = chapter.startTime;
                options.end = chapter.endTime;
            }
        }
    }
    OFS::Tooltip("The chapter under the playhead.");
    ImGui::SameLine();
    if (ImGui::SmallButton("30 s from playhead")) {
        options.start = (float)app->player->CurrentTime();
        options.end = Util::Min((float)app->player->Duration(), options.start + 30.f);
    }
    const float length = Util::Max(0.f, options.end - options.start);
    ImGui::TextDisabled("%s to %s, %s long", formatSeconds(options.start).c_str(),
        formatSeconds(options.end).c_str(), formatSeconds(length).c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("Format");
    static constexpr const char* formatTips[3] = {
        "A video with the sound. Plays everywhere, and uploads to EroScripts as it is.",
        "An animated image with no sound that loops by itself in a post. Best kept short.",
        "The same as WebP for places that only take GIF. Larger, and fewer colours.",
    };
    const int32_t pickedFormat = OFS::SegmentedControl("##PreviewFormat", FormatNames, formatTips, 3, options.format);
    if (pickedFormat >= 0) options.format = pickedFormat;
    static constexpr const char* sizeLabels[3] = { "480p", "720p", "1080p" };
    static constexpr const char* sizeTips[3] = { "Small and quick.", "The usual size for a post.", "Full HD; the largest file." };
    int32_t sizeIndex = options.height <= 480 ? 0 : options.height <= 720 ? 1 : 2;
    const int32_t pickedSize = OFS::SegmentedControl("##PreviewSize", sizeLabels, sizeTips, 3, sizeIndex);
    if (pickedSize >= 0) options.height = Heights[pickedSize];
    if (options.format != Mp4 && length > 30.f) {
        ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::Role().AccentPale);
        ImGui::TextWrapped("Animated images of more than 30 seconds get large. MP4 suits long clips.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    const bool nothing = !options.video && !options.simulator && !options.timeline && !options.heatmap;
    ImGui::BeginDisabled(length < 0.1f || nothing);
    if (ImGui::Button("Export...")) {
        auto& project = app->LoadedProject;
        auto base = Util::PathFromString(app->player->VideoPath() ? app->player->VideoPath() : project->Path());
        base.replace_extension("");
        const std::string suggested = base.u8string() + ".preview" + FormatExtensions[options.format];
        const int32_t format = options.format;
        Util::SaveFileDialog("Save preview video", suggested,
            [this](auto& result) {
                if (result.files.empty()) return;
                outputPath = result.files.front();
                if (!start()) stage = Stage::Failed;
            },
            { format == Mp4 ? "*.mp4" : format == Webp ? "*.webp" : "*.gif" }, FormatNames[format]);
        dialogOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        dialogOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void OFS_PreviewExport::showProgress() noexcept
{
    if (stage == Stage::Idle) return;
    const ImGuiViewport* main = ImGui::GetMainViewport();
    const float pad = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(main->WorkPos.x + main->WorkSize.x - pad, main->WorkPos.y + main->WorkSize.y - pad),
        ImGuiCond_Always, ImVec2(1.f, 1.f));
    ImGui::SetNextWindowSize(ImVec2(pad * 20.f, 0.f), ImGuiCond_Always);
    if (!ImGui::Begin(ProgressId, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        return;
    }
    const std::string name = Util::PathFromString(outputPath).filename().u8string();
    switch (stage) {
        case Stage::Rendering:
        case Stage::Finishing: {
            const float done = framesTotal > 0 ? (float)framesDone / (float)framesTotal : 0.f;
            ImGui::TextUnformatted(stage == Stage::Finishing ? "Finishing the file..." : "Rendering the preview...");
            char overlay[48];
            snprintf(overlay, sizeof(overlay), "%d / %d frames", framesDone, framesTotal);
            ImGui::ProgressBar(done, ImVec2(-1.f, 0.f), overlay);
            if (ImGui::Button("Cancel")) finish(true);
            break;
        }
        case Stage::Done:
            ImGui::TextWrapped("Saved %s", name.c_str());
            if (ImGui::Button("Show in folder")) {
                auto folder = Util::PathFromString(outputPath);
                folder.remove_filename();
                Util::OpenFileExplorer(folder.u8string());
            }
            ImGui::SameLine();
            if (ImGui::Button("Close")) stage = Stage::Idle;
            break;
        case Stage::Failed:
            ImGui::PushStyleColor(ImGuiCol_Text, OFS_Sashimi::Role().AccentPale);
            ImGui::TextWrapped("%s", failure.empty() ? "The preview could not be made." : failure.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Close")) stage = Stage::Idle;
            break;
        default:
            break;
    }
    ImGui::End();
}
