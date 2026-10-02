#pragma once

#include "OFS_StateHandle.h"
#include "Funscript.h"

#include <cmath>
#include <vector>
#include <string>

struct TempoOverlayState
{
    static constexpr auto StateName = "TempoOverlayState";

    float bpm = 100.f;
    float beatOffsetSeconds = 0.f;
    uint32_t measureIndex = 0;
    // Follow the chapter under the playhead instead of holding one tempo for
    // the whole media. On by default: with it off, every track in a mix needs
    // its tempo applied to the grid by hand, which is the work the chapters
    // were measured to avoid. An existing project keeps whatever it was saved
    // with, since this is only the value a new one starts from.
    bool autoTempo = true;

    // Takes bpm and phase from a detected tempo.
    //
    // The offset is reduced into a single bar. The grid uses it modulo its own
    // line spacing and every spacing on offer divides a four beat bar exactly,
    // so the phase is identical for all of them, while the value stays inside
    // the range the offset drag accepts rather than being an absolute time
    // hundreds of seconds into the media.
    inline void SetFromTempo(float newBpm, float measureOffsetSeconds) noexcept
    {
        if(newBpm <= 0.f) return;
        bpm = newBpm;
        const float barSeconds = (60.f / newBpm) * 4.f;
        float offset = std::fmod(measureOffsetSeconds, barSeconds);
        if(offset < 0.f) offset += barSeconds;
        beatOffsetSeconds = offset;
    }

    // Rotates the grid by whole beats. Detection picks the downbeat from onset
    // energy alone, which cannot separate the four beats of a bar when they are
    // equally loud, so this is how that gets corrected by ear.
    inline void NudgeDownbeat(int32_t beats) noexcept
    {
        if(bpm <= 0.f || beats == 0) return;
        const float beatSeconds = 60.f / bpm;
        const float barSeconds = beatSeconds * 4.f;
        float offset = std::fmod(beatOffsetSeconds + (beatSeconds * (float)beats), barSeconds);
        if(offset < 0.f) offset += barSeconds;
        beatOffsetSeconds = offset;
    }

    // Re-phases the grid so a bar line lands exactly on the given time. The
    // detector cannot always find the downbeat from onset energy alone, so this
    // is the way to put it right by ear: park the playhead on a beat you can
    // hear and say "there".
    inline void SetDownbeatAt(float time) noexcept
    {
        if(bpm <= 0.f) return;
        const float barSeconds = (60.f / bpm) * 4.f;
        float offset = std::fmod(time, barSeconds);
        if(offset < 0.f) offset += barSeconds;
        beatOffsetSeconds = offset;
    }

    inline static TempoOverlayState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<TempoOverlayState>(stateHandle).Get();
    }
};

REFL_TYPE(TempoOverlayState)
    REFL_FIELD(bpm)
    REFL_FIELD(beatOffsetSeconds)
    REFL_FIELD(measureIndex)
    REFL_FIELD(autoTempo)
REFL_END

struct ProjectState
{
    static constexpr auto StateName = "ProjectState";

    Funscript::Metadata metadata;
    std::string relativeMediaPath;
    // How long the timeline runs when there is no media to take a length from.
    // Zero for a project backed by a video or an audio file, which carry their
    // own. See OFS_Project::CreateStandalone.
    float standaloneDuration = 0.f;
    float activeTimer = 0.f;
    float lastPlayerPosition = 0.f;
    uint32_t activeScriptIdx = 0;
    bool nudgeMetadata = true;

    std::vector<uint8_t> binaryFunscriptData;

    inline static ProjectState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<ProjectState>(stateHandle).Get();
    }
};

REFL_TYPE(ProjectState)
    REFL_FIELD(metadata)
    REFL_FIELD(relativeMediaPath)
    REFL_FIELD(standaloneDuration)
    REFL_FIELD(activeTimer)
    REFL_FIELD(lastPlayerPosition)
    REFL_FIELD(activeScriptIdx)
    REFL_FIELD(nudgeMetadata)
    REFL_FIELD(binaryFunscriptData)
REFL_END

