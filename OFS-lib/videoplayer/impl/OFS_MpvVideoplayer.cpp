#include "OFS_Videoplayer.h"
#include "OFS_Util.h"

#include "OFS_EventSystem.h"
#include "OFS_VideoplayerEvents.h"

#define OFS_MPV_LOADER_MACROS
#include "OFS_MpvLoader.h"

#include "OFS_Localization.h"
#include "OFS_GL.h"

#include <cmath>
#include <sstream>

#include "SDL_timer.h"
#include "SDL_atomic.h"

enum MpvPropertyGet : uint64_t {
    MpvDuration,
    MpvPosition,
    MpvTotalFrames,
    MpvSpeed,
    MpvVideoWidth,
    MpvVideoHeight,
    MpvPauseState,
    MpvFilePath,
    MpvHwDecoder,
    MpvFramesPerSecond,
};

struct MpvDataCache {
    double duration = 1.0;
    double percentPos = 0.0;
    double currentSpeed = 1.0;
    double fps = 30.0;
    double averageFrameTime = 1.0/fps;
    
    double abLoopA = 0;
    double abLoopB = 0;

    int64_t totalNumFrames = 0;
    int64_t paused = false;
    int64_t videoWidth = 0;
    int64_t videoHeight = 0;

    float currentVolume = .5f;

    bool videoLoaded = false;
    // No media is open and the position below is advanced by OFS_Videoplayer
    // itself. See OpenBlank.
    bool blank = false;
    // The file loaded has sound and nothing to look at. A song with cover art
    // is not this: mpv shows the picture as its video. See handleTrackReply.
    bool audioOnly = false;
    // The picture is a still, most often a song's cover art. Its frame rate
    // says nothing about the sound and is usually 1, which would make a frame
    // step a whole second, so a fixed one is used instead.
    bool stillPicture = false;
    std::string filePath = "";
};

struct MpvPlayerContext
{
    mpv_handle* mpv = nullptr;
    mpv_render_context* mpvGL = nullptr;
    uint32_t framebuffer = 0;
    MpvDataCache data = MpvDataCache();

    std::array<char, 32> tmpBuf;
    SDL_atomic_t renderUpdate = {0};
    SDL_atomic_t hasEvents = {0};

    uint32_t* frameTexture = nullptr;
    float* logicalPosition = nullptr;

    uint64_t smoothTimer = 0;
    // Counts files opened and closed, to tell replies about an old file apart.
    uint64_t loadGeneration = 1;
    VideoplayerType playerType;
};

#define CTX static_cast<MpvPlayerContext*>(ctx)

static void OnMpvEvents(void* ctx) noexcept
{
    SDL_AtomicIncRef(&CTX->hasEvents);
}

static void OnMpvRenderUpdate(void* ctx) noexcept
{
    SDL_AtomicIncRef(&CTX->renderUpdate);
}

inline static void notifyVideoLoaded(MpvPlayerContext* ctx) noexcept
{
    EV::Enqueue<VideoLoadedEvent>(CTX->data.filePath, CTX->playerType);
}

inline static void notifyPaused(MpvPlayerContext* ctx) noexcept
{
    EV::Enqueue<PlayPauseChangeEvent>(CTX->data.paused, CTX->playerType);
}

inline static void notifyTime(MpvPlayerContext* ctx) noexcept
{
    EV::Enqueue<TimeChangeEvent>((float)(CTX->data.duration * CTX->data.percentPos), CTX->playerType);
}

inline static void notifyDuration(MpvPlayerContext* ctx) noexcept
{
    EV::Enqueue<DurationChangeEvent>((float)CTX->data.duration, CTX->playerType);   
}

inline static void notifyPlaybackSpeed(MpvPlayerContext* ctx) noexcept
{
    EV::Enqueue<PlaybackSpeedChangeEvent>((float)CTX->data.currentSpeed, CTX->playerType);
}

inline static void updateRenderTexture(MpvPlayerContext* ctx) noexcept
{
    if (!ctx->framebuffer) {
		glGenFramebuffers(1, &ctx->framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, ctx->framebuffer);

		glGenTextures(1, ctx->frameTexture);
		glBindTexture(GL_TEXTURE_2D, *ctx->frameTexture);
		
        int initialWidth = ctx->data.videoWidth > 0 ? ctx->data.videoWidth : 1920;
		int initialHeight = ctx->data.videoHeight > 0 ? ctx->data.videoHeight : 1080;
		glTexImage2D(GL_TEXTURE_2D, 0, OFS_InternalTexFormat, initialWidth, initialHeight, 0, OFS_TexFormat, GL_UNSIGNED_BYTE, 0);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Set "renderedTexture" as our colour attachement #0
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *ctx->frameTexture, 0);

		// Set the list of draw buffers.
		GLenum DrawBuffers[1] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers(1, DrawBuffers); 

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOG_ERROR("Failed to create framebuffer for video!");
		}
	}
	else if(ctx->data.videoHeight > 0 && ctx->data.videoWidth > 0) {
		// update size of render texture based on video resolution
		glBindTexture(GL_TEXTURE_2D, *ctx->frameTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, OFS_InternalTexFormat, ctx->data.videoWidth, ctx->data.videoHeight, 0, OFS_TexFormat, GL_UNSIGNED_BYTE, 0);
	}
}

// Moves a blank timeline's position forward by however long it has been
// playing since this last ran, and restarts that clock. The mpv path gets the
// same thing from percent-pos events; here nothing reports it, so every read
// or change of the position has to settle up first.
inline static void advanceBlankClock(MpvPlayerContext* ctx, float* logicalPosition) noexcept
{
    uint64_t now = SDL_GetTicks64();
    if (!ctx->data.paused) {
        float elapsed = (now - ctx->smoothTimer) / 1000.f;
        *logicalPosition += (float)((elapsed * ctx->data.currentSpeed) / ctx->data.duration);
        // Matches mpv's loop-file=inf, which is what a video does at the end.
        if (*logicalPosition >= 1.f) *logicalPosition -= std::floor(*logicalPosition);
        else if (*logicalPosition < 0.f) *logicalPosition = 0.f;
        ctx->data.percentPos = *logicalPosition;
    }
    ctx->smoothTimer = now;
}

inline static void showText(MpvPlayerContext* ctx, const char* text) noexcept
{
    const char* cmd[] = { "show_text", text, NULL };
    mpv_command_async(ctx->mpv, 0, cmd);
}

OFS_Videoplayer::~OFS_Videoplayer() noexcept
{
    mpv_render_context_free(CTX->mpvGL);
	mpv_destroy(CTX->mpv);
    delete CTX;
    ctx = nullptr;
}

OFS_Videoplayer::OFS_Videoplayer(VideoplayerType playerType) noexcept
{
    this->playerType = playerType;
    ctx = new MpvPlayerContext();
    CTX->playerType = playerType;
    CTX->frameTexture = &this->frameTexture;
    CTX->logicalPosition = &this->logicalPosition;
}

bool OFS_Videoplayer::Init(bool hwAccel) noexcept
{
    CTX->mpv = mpv_create();
    if(!CTX->mpv) {
        return false;
    }
    auto confPath = Util::Prefpath();

    int error = 0;
    error = mpv_set_option_string(CTX->mpv, "config", "yes");
    if(error != 0) {
        LOG_WARN("Failed to set mpv: config=yes");
    }
    error = mpv_set_option_string(CTX->mpv, "config-dir", confPath.c_str());
    if(error != 0) {
        LOGF_WARN("Failed to set mpv: config-dir=%s", confPath.c_str());
    }

    if(mpv_initialize(CTX->mpv) != 0) {
        return false;
    }

    error = mpv_set_property_string(CTX->mpv, "loop-file", "inf");
    if(error != 0) {
        LOG_WARN("Failed to set mpv: loop-file=inf");
    }

    if(hwAccel) {
        error = mpv_set_property_string(CTX->mpv, "profile", "gpu-hq");
        if(error != 0) {
            LOG_WARN("Failed to set mpv: profile=gpu-hq");
        }
        error = mpv_set_property_string(CTX->mpv, "hwdec", "auto-safe");
        if(error != 0) {
            LOG_WARN("Failed to set mpv: hwdec=auto-safe");
        }
    }
    else {
        error = mpv_set_property_string(CTX->mpv, "hwdec", "no");
        if(error != 0) {
            LOG_WARN("Failed to set mpv: hwdec=no");
        }
    }

#ifndef NDEBUG
    mpv_request_log_messages(CTX->mpv, "debug");
#else
    mpv_request_log_messages(CTX->mpv, "info");
#endif

    mpv_opengl_init_params init_params = {0};
	init_params.get_proc_address = [](void* mpvContext, const char* fnName) noexcept -> void*
    {
        return SDL_GL_GetProcAddress(fnName);
    };
    
    uint32_t enable = 1;
	mpv_render_param renderParams[] = {
		mpv_render_param{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL},
		mpv_render_param{MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init_params},
		mpv_render_param{MPV_RENDER_PARAM_ADVANCED_CONTROL, &enable },
		mpv_render_param{}
	};

    if (mpv_render_context_create(&CTX->mpvGL, CTX->mpv, renderParams) < 0) {
		LOG_ERROR("Failed to initialize mpv GL context");
		return false;
	}

    mpv_set_wakeup_callback(CTX->mpv, OnMpvEvents, ctx);
    mpv_render_context_set_update_callback(CTX->mpvGL, OnMpvRenderUpdate, ctx);

	mpv_observe_property(CTX->mpv, MpvVideoHeight, "height", MPV_FORMAT_INT64);
	mpv_observe_property(CTX->mpv, MpvVideoWidth, "width", MPV_FORMAT_INT64);
	mpv_observe_property(CTX->mpv, MpvDuration, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(CTX->mpv, MpvPosition, "percent-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(CTX->mpv, MpvTotalFrames, "estimated-frame-count", MPV_FORMAT_INT64);
	mpv_observe_property(CTX->mpv, MpvSpeed, "speed", MPV_FORMAT_DOUBLE);
	mpv_observe_property(CTX->mpv, MpvPauseState, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(CTX->mpv, MpvFilePath, "path", MPV_FORMAT_STRING);
	mpv_observe_property(CTX->mpv, MpvHwDecoder, "hwdec-current", MPV_FORMAT_STRING);
	mpv_observe_property(CTX->mpv, MpvFramesPerSecond, "estimated-vf-fps", MPV_FORMAT_DOUBLE);

    return true;
}

// The frame rate used when the file has no moving picture to take one from.
static constexpr double NoMotionFps = 30.0;

// What is asked about the video track. Packed with the load generation into a
// reply's userdata, which is the only thing a reply carries back.
enum class TrackQuery : uint64_t
{
    Present = 0,
    Still = 1,
};

inline static uint64_t trackQueryTag(MpvPlayerContext* ctx, TrackQuery query) noexcept
{
    return (ctx->loadGeneration << 1) | (uint64_t)query;
}

// Tracks are chosen by the time a file has loaded, so this is asked then. It
// has to be asked asynchronously: a blocking get waits on mpv, which for a file
// with a picture is itself waiting on this thread to set up rendering, and the
// app hangs. Replies are tagged with the load they were asked for, so one that
// comes back after another file was opened is ignored.
inline static void queryVideoTrack(MpvPlayerContext* ctx) noexcept
{
    mpv_get_property_async(ctx->mpv, trackQueryTag(ctx, TrackQuery::Present),
        "current-tracks/video/id", MPV_FORMAT_INT64);
    mpv_get_property_async(ctx->mpv, trackQueryTag(ctx, TrackQuery::Still),
        "current-tracks/video/image", MPV_FORMAT_FLAG);
}

inline static void useFixedFrameRate(MpvPlayerContext* ctx) noexcept
{
    ctx->data.fps = NoMotionFps;
    ctx->data.averageFrameTime = 1.0 / NoMotionFps;
    ctx->data.totalNumFrames = (int64_t)(ctx->data.duration * NoMotionFps);
}

inline static void handleTrackReply(MpvPlayerContext* ctx, mpv_event* ev) noexcept
{
    if ((ev->reply_userdata >> 1) != ctx->loadGeneration || ctx->data.blank) return;
    const auto query = (TrackQuery)(ev->reply_userdata & 1);

    if (query == TrackQuery::Present) {
        // No selected video track is reported as the property being
        // unavailable. Any other failure is taken to mean there is a picture,
        // which is how every file was treated before this was asked.
        ctx->data.audioOnly = ev->error == MPV_ERROR_PROPERTY_UNAVAILABLE;
        if (ctx->data.audioOnly) useFixedFrameRate(ctx);
        return;
    }

    if (ev->error < 0) return;
    auto prop = (mpv_event_property*)ev->data;
    if (prop == nullptr || prop->format != MPV_FORMAT_FLAG || prop->data == nullptr) return;
    ctx->data.stillPicture = *(int*)prop->data != 0;
    if (ctx->data.stillPicture) useFixedFrameRate(ctx);
}

inline static void ProcessEvents(MpvPlayerContext* ctx) noexcept
{
    for(;;) {
		mpv_event* mp_event = mpv_wait_event(ctx->mpv, 0.);
		if (mp_event->event_id == MPV_EVENT_NONE)
			break;
			
		switch (mp_event->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
            {
                mpv_event_log_message* msg = (mpv_event_log_message*)mp_event->data;
                char MpvLogPrefix[48];
                int len = stbsp_snprintf(MpvLogPrefix, sizeof(MpvLogPrefix), "[%s][MPV] (%s): ", msg->level, msg->prefix);
                FUN_ASSERT(len <= sizeof(MpvLogPrefix), "buffer to small");
                OFS_FileLogger::LogToFileR(MpvLogPrefix, msg->text);
                continue;
            }
            case MPV_EVENT_COMMAND_REPLY:
            {
                // attach user_data to command
                // and handle it here when it finishes
                continue;
            }
            case MPV_EVENT_FILE_LOADED:
            {
                if (ctx->data.blank) continue;
                ctx->data.videoLoaded = true; 	
                queryVideoTrack(ctx);
                continue;
            }
            case MPV_EVENT_GET_PROPERTY_REPLY:
            {
                // Only the video track is ever asked about this way.
                handleTrackReply(ctx, mp_event);
                continue;
            }
            case MPV_EVENT_PROPERTY_CHANGE:
            {
                mpv_event_property* prop = (mpv_event_property*)mp_event->data;
                if (prop->data == nullptr) break;
                // On a blank timeline mpv is stopped and the cache below is
                // ours, not its. Its properties are leftovers from the last
                // file and would undo what the blank clock just set.
                if (ctx->data.blank) break;
                switch (mp_event->reply_userdata) {
                    case MpvHwDecoder:
                    {
                        LOGF_INFO("Active hardware decoder: %s", *(char**)prop->data);
                        break;
                    }
                    case MpvVideoWidth:
                    {
                        ctx->data.videoWidth = *(int64_t*)prop->data;
                        if (ctx->data.videoHeight > 0.f) {
                            updateRenderTexture(ctx);
                            ctx->data.videoLoaded = true;
                        }
                        break;
                    }
                    case MpvVideoHeight:
                    {
                        ctx->data.videoHeight = *(int64_t*)prop->data;
                        if (ctx->data.videoWidth > 0.f) {
                            updateRenderTexture(ctx);
                            ctx->data.videoLoaded = true;
                        }
                        break;
                    }
                    case MpvFramesPerSecond:
                        if (ctx->data.stillPicture || ctx->data.audioOnly) break;
                        ctx->data.fps = *(double*)prop->data;
                        ctx->data.averageFrameTime = (1.0 / ctx->data.fps);
                        break;
                    case MpvDuration:
                        ctx->data.duration = *(double*)prop->data;
                        if (ctx->data.stillPicture || ctx->data.audioOnly) useFixedFrameRate(ctx);
                        notifyDuration(ctx);
                        break;
                    case MpvTotalFrames:
                        if (ctx->data.stillPicture || ctx->data.audioOnly) break;
                        ctx->data.totalNumFrames = *(int64_t*)prop->data;
                        break;
                    case MpvPosition:
                    {
                        auto newPercentPos = (*(double*)prop->data) / 100.0;
                        ctx->data.percentPos = newPercentPos;
                        ctx->smoothTimer = SDL_GetTicks64();
                        if(!ctx->data.paused) {
                            *ctx->logicalPosition = newPercentPos;
                        }
                        notifyTime(ctx);
                        break;
                    }
                    case MpvSpeed:
                        ctx->data.currentSpeed = *(double*)prop->data;
                        notifyPlaybackSpeed(ctx);
                        break;
                    case MpvPauseState:
                    {
                        bool paused = *(int64_t*)prop->data;
                        // Only time that actually played is banked. mpv reports
                        // its pause at startup too, with nothing loaded and the
                        // cache still saying it plays, which used to push the
                        // idle position a fraction of the way along.
                        if (paused && !ctx->data.paused && ctx->data.videoLoaded) {
                            float timeSinceLastUpdate = (SDL_GetTicks64() - CTX->smoothTimer) / 1000.f;
                            float positionOffset = (timeSinceLastUpdate * CTX->data.currentSpeed) / CTX->data.duration;
                            *ctx->logicalPosition += positionOffset;
                        }
                        ctx->smoothTimer = SDL_GetTicks64();
                        ctx->data.paused = paused;
                        notifyPaused(ctx);
                        break;
                    }
                    case MpvFilePath:
                        ctx->data.filePath = *((const char**)(prop->data));
                        notifyVideoLoaded(ctx);
                        break;
                }
                continue;
            }
		}
	}
}

inline static void RenderFrameToTexture(MpvPlayerContext* ctx) noexcept
{
    mpv_opengl_fbo fbo = {0};
	fbo.fbo = ctx->framebuffer; 
	fbo.w = ctx->data.videoWidth;
	fbo.h = ctx->data.videoHeight;
	fbo.internal_format = OFS_InternalTexFormat;

	uint32_t disable = 0;
	mpv_render_param params[] = {
		{MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
		{MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &disable}, 
		mpv_render_param{}
	};
	mpv_render_context_render(ctx->mpvGL, params);
}

void OFS_Videoplayer::Update(float delta) noexcept
{
    while(SDL_AtomicGet(&CTX->hasEvents) > 0) {
        ProcessEvents(CTX);
        SDL_AtomicDecRef(&CTX->hasEvents);
    }

    while(SDL_AtomicGet(&CTX->renderUpdate) > 0)
    {
        uint64_t flags = mpv_render_context_update(CTX->mpvGL);
	    if (flags & MPV_RENDER_UPDATE_FRAME) {
            RenderFrameToTexture(CTX);
        }
        SDL_AtomicDecRef(&CTX->renderUpdate);
    }

    if (CTX->data.blank && !CTX->data.paused) {
        advanceBlankClock(CTX, &logicalPosition);
        notifyTime(CTX);
    }
}

void OFS_Videoplayer::OpenBlank(float durationSeconds) noexcept
{
    LOGF_INFO("Opening blank timeline of %.1f seconds", durationSeconds);
    CloseVideo();

    MpvDataCache newCache;
    newCache.currentSpeed = CTX->data.currentSpeed;
    newCache.currentVolume = CTX->data.currentVolume;
    newCache.blank = true;
    newCache.videoLoaded = true;
    newCache.paused = true;
    newCache.duration = Util::Clamp(durationSeconds, MinBlankDuration, MaxBlankDuration);
    // Nothing decides the frame rate here, so it is picked: fine enough that a
    // frame step is a small step, and a round number so the frame and the
    // timeline's millisecond grid line up.
    newCache.fps = 60.0;
    newCache.averageFrameTime = 1.0 / newCache.fps;
    newCache.totalNumFrames = (int64_t)(newCache.duration * newCache.fps);
    CTX->data = newCache;

    logicalPosition = 0.f;
    CTX->smoothTimer = SDL_GetTicks64();

    notifyDuration(CTX);
    // With an empty path, which is what tells everything listening that there
    // is no file here to read a waveform or a thumbnail out of.
    notifyVideoLoaded(CTX);
    notifyPaused(CTX);
    notifyTime(CTX);
}

void OFS_Videoplayer::SetBlankDuration(float durationSeconds) noexcept
{
    if (!CTX->data.blank) return;
    durationSeconds = Util::Clamp(durationSeconds, MinBlankDuration, MaxBlankDuration);
    if (CTX->data.duration == durationSeconds) return;

    advanceBlankClock(CTX, &logicalPosition);
    // The playhead is held where it is in seconds rather than as a fraction, so
    // changing the length does not drag it along with it.
    double timeSeconds = logicalPosition * CTX->data.duration;
    CTX->data.duration = durationSeconds;
    logicalPosition = Util::Clamp((float)(timeSeconds / durationSeconds), 0.f, 1.f);
    CTX->data.percentPos = logicalPosition;
    CTX->data.totalNumFrames = (int64_t)(CTX->data.duration * CTX->data.fps);

    notifyDuration(CTX);
    notifyTime(CTX);
}

void OFS_Videoplayer::SetVolume(float volume) noexcept
{
    CTX->data.currentVolume = volume;
    if (CTX->data.blank) return;
    stbsp_snprintf(CTX->tmpBuf.data(), CTX->tmpBuf.size(), "%.2f", (float)(volume*100.f));
    const char* cmd[]{"set", "volume", CTX->tmpBuf.data(), NULL};
    mpv_command_async(CTX->mpv, 0, cmd);
}

void OFS_Videoplayer::NextFrame() noexcept
{
    if (IsPaused()) {
        // use same method as previousFrame for consistency
        double relSeek = FrameTime() * 1.000001;
        CTX->data.percentPos += (relSeek / CTX->data.duration);
        CTX->data.percentPos = Util::Clamp(CTX->data.percentPos, 0.0, 1.0);
        SetPositionPercent(CTX->data.percentPos, false);
    }
}

void OFS_Videoplayer::PreviousFrame() noexcept
{
    if (IsPaused()) {
        // this seeks much faster
        // https://github.com/mpv-player/mpv/issues/4019#issuecomment-358641908
        double relSeek = FrameTime() * 1.000001;
        CTX->data.percentPos -= (relSeek / CTX->data.duration);
        CTX->data.percentPos = Util::Clamp(CTX->data.percentPos, 0.0, 1.0);
        SetPositionPercent(CTX->data.percentPos, false);
    }
}

void OFS_Videoplayer::OpenVideo(const std::string& path) noexcept
{
    LOGF_INFO("Opening video: \"%s\"", path.c_str());
    CloseVideo();
    
    const char* cmd[] = { "loadfile", path.c_str(), NULL };
    mpv_command_async(CTX->mpv, 0, cmd);
    
    MpvDataCache newCache;
    newCache.currentSpeed = CTX->data.currentSpeed;
    newCache.paused = CTX->data.paused;
    CTX->data = newCache;

    SetPaused(true);
    SetVolume(CTX->data.currentVolume);
    SetSpeed(CTX->data.currentSpeed);
}

void OFS_Videoplayer::SetSpeed(float speed) noexcept
{
    speed = Util::Clamp<float>(speed, MinPlaybackSpeed, MaxPlaybackSpeed);
    if (CTX->data.blank) {
        if (CTX->data.currentSpeed == speed) return;
        // Whatever has played so far played at the old speed.
        advanceBlankClock(CTX, &logicalPosition);
        CTX->data.currentSpeed = speed;
        notifyPlaybackSpeed(CTX);
        return;
    }
    if (CurrentSpeed() != speed) {
        stbsp_snprintf(CTX->tmpBuf.data(), CTX->tmpBuf.size(), "%.3f", speed);
        const char* cmd[]{ "set", "speed", CTX->tmpBuf.data(), NULL };
        mpv_command_async(CTX->mpv, 0, cmd);
    }
}

void OFS_Videoplayer::AddSpeed(float speed) noexcept
{
    speed += CTX->data.currentSpeed;
    speed = Util::Clamp<float>(speed, MinPlaybackSpeed, MaxPlaybackSpeed);
    SetSpeed(speed);
}

void OFS_Videoplayer::SetPositionPercent(float percentPos, bool pausesVideo) noexcept
{
    if (CTX->data.blank) {
        if (pausesVideo) SetPaused(true);
        percentPos = Util::Clamp(percentPos, 0.f, 1.f);
        logicalPosition = percentPos;
        CTX->data.percentPos = percentPos;
        CTX->smoothTimer = SDL_GetTicks64();
        notifyTime(CTX);
        return;
    }
    logicalPosition = percentPos;
    CTX->data.percentPos = percentPos;
    stbsp_snprintf(CTX->tmpBuf.data(), CTX->tmpBuf.size(), "%.08f", (float)(percentPos * 100.0f));
    const char* cmd[]{ "seek", CTX->tmpBuf.data(), "absolute-percent+exact", NULL };
    if (pausesVideo) {
        SetPaused(true);
    }
    mpv_command_async(CTX->mpv, 0, cmd);
}

void OFS_Videoplayer::SetPositionExact(float timeSeconds, bool pausesVideo) noexcept
{
    // this updates logicalPosition in SetPositionPercent
    timeSeconds = Util::Clamp<float>(timeSeconds, 0.f, Duration());
    float relPos = ((float)timeSeconds) / Duration();
    SetPositionPercent(relPos, pausesVideo);
}

void OFS_Videoplayer::SeekRelative(float timeSeconds) noexcept
{
    // this updates logicalPosition in SetPositionPercent
    auto seekTo = CurrentTime() + timeSeconds;
    seekTo = std::max(seekTo, 0.0);
    SetPositionExact(seekTo);
}

void OFS_Videoplayer::SeekFrames(int32_t offset) noexcept
{
    // this updates logicalPosition in SetPositionPercent
    if (IsPaused()) {
        float relSeek = (FrameTime() * 1.000001f) * offset;
        CTX->data.percentPos += (relSeek / CTX->data.duration);
        CTX->data.percentPos = Util::Clamp(CTX->data.percentPos, 0.0, 1.0);
        SetPositionPercent(CTX->data.percentPos, false);
    }
}

void OFS_Videoplayer::SetPaused(bool paused) noexcept
{
    if ((bool)CTX->data.paused == paused) return;
    if (CTX->data.blank) {
        // Bank the time played before the pause takes effect.
        advanceBlankClock(CTX, &logicalPosition);
        CTX->data.paused = paused;
        notifyPaused(CTX);
        return;
    }
    int64_t setPaused = paused;
    mpv_set_property_async(CTX->mpv, 0, "pause", MPV_FORMAT_FLAG, &setPaused);
}

void OFS_Videoplayer::CycleSubtitles() noexcept
{
    if (CTX->data.blank) return;
    const char* cmd[]{ "cycle", "sub", NULL};
    mpv_command_async(CTX->mpv, 0, cmd);
}

void OFS_Videoplayer::CloseVideo() noexcept
{
    CTX->data.videoLoaded = false;
    CTX->data.blank = false;
    CTX->data.audioOnly = false;
    CTX->data.stillPicture = false;
    CTX->loadGeneration++;
    const char* cmd[] = { "stop", NULL };
    mpv_command_async(CTX->mpv, 0, cmd);
    SetPaused(true);
}

void OFS_Videoplayer::NotifySwap() noexcept
{
    mpv_render_context_report_swap(CTX->mpvGL);
}

void OFS_Videoplayer::SaveFrameToImage(const std::string& directory) noexcept
{
    if (CTX->data.blank) return;
    std::stringstream ss;
    auto currentFile = Util::PathFromString(VideoPath());
    std::string filename = currentFile.filename().replace_extension("").string();
    std::array<char, 15> tmp;
    double time = CurrentTime();
    Util::FormatTime(tmp.data(), tmp.size(), time, true);
    std::replace(tmp.begin(), tmp.end(), ':', '_');
    ss << filename << '_' << tmp.data() << ".png";
    if(!Util::CreateDirectories(directory)) {
        return;
    }
    auto dir = Util::PathFromString(directory);
    dir.make_preferred();
    std::string finalPath = (dir / ss.str()).string();
    const char* cmd[]{ "screenshot-to-file", finalPath.c_str(), NULL };
    mpv_command_async(CTX->mpv, 0, cmd);
}

// ==================== Getter ==================== 

uint16_t OFS_Videoplayer::VideoWidth() const noexcept
{
    return CTX->data.videoWidth;
}

uint16_t OFS_Videoplayer::VideoHeight() const noexcept
{
    return CTX->data.videoHeight;
}

float OFS_Videoplayer::FrameTime() const noexcept
{
    return CTX->data.averageFrameTime;
}

float OFS_Videoplayer::CurrentSpeed() const noexcept
{
    return CTX->data.currentSpeed;
}

float OFS_Videoplayer::Volume() const noexcept
{
    return CTX->data.currentVolume;
}

double OFS_Videoplayer::Duration() const noexcept
{
    return CTX->data.duration;
}

bool OFS_Videoplayer::IsPaused() const noexcept
{
    return CTX->data.paused;
}

float OFS_Videoplayer::Fps() const noexcept
{
    return CTX->data.fps;
}

bool OFS_Videoplayer::VideoLoaded() const noexcept
{
    return CTX->data.videoLoaded;
}

bool OFS_Videoplayer::IsBlank() const noexcept
{
    return CTX->data.blank;
}

bool OFS_Videoplayer::IsAudioOnly() const noexcept
{
    return CTX->data.videoLoaded && !CTX->data.blank && CTX->data.audioOnly;
}

bool OFS_Videoplayer::HasVisual() const noexcept
{
    return CTX->data.videoWidth > 0 && CTX->data.videoHeight > 0;
}

float OFS_Videoplayer::CurrentPercentPosition() const noexcept
{
    return logicalPosition;
}

double OFS_Videoplayer::CurrentTime() const noexcept
{
    if(CTX->data.paused)
    {
        return logicalPosition * CTX->data.duration;
    }
    else 
    {
        float timeSinceLastUpdate = (SDL_GetTicks64() - CTX->smoothTimer) / 1000.f;
        float positionOffset = (timeSinceLastUpdate * CTX->data.currentSpeed) / Duration();
        return (logicalPosition + positionOffset) * CTX->data.duration;
    }
}

double OFS_Videoplayer::CurrentPlayerPosition() const noexcept
{
    return CTX->data.percentPos;
}

const char* OFS_Videoplayer::VideoPath() const noexcept
{
    return CTX->data.filePath.c_str();
}
