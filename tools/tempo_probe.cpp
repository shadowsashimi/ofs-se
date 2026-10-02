// ============================================================================
//  Checks for OFS_TempoDetection against synthetic click tracks, where the
//  right answer is known exactly rather than argued about by ear.
//
//  Build and run with tools\tempo-probe.ps1. Prints a line per check and exits
//  non-zero if any of them fail.
//
//  The envelope here stands in for the timeline waveform OFS hands the
//  detector: an amplitude envelope at roughly 160 Hz. A click track's envelope
//  is a spike on every beat decaying to nothing before the next, which is as
//  clean a signal as a beat can be. Anything the detector gets wrong on this,
//  it gets wrong on music too, just less visibly.
// ============================================================================

#include "OFS_TempoDetection.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace
{

constexpr float EnvelopeRate = 160.f;
constexpr int32_t BeatsPerBar = 4;

// Two envelope samples. A phase can only be resolved to the sample, so asking
// for better than that would be testing the rounding.
constexpr float PhaseToleranceSeconds = 2.f / EnvelopeRate;
constexpr float BpmTolerance = 0.25f;

int32_t failures = 0;
int32_t checks = 0;

void expect(bool ok, const char* name, const char* fmt, ...)
{
    checks += 1;
    if(!ok) failures += 1;
    std::printf("  %s  %-46s ", ok ? "pass" : "FAIL", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

// Appends a click track to the envelope. accentBeat, when set, doubles the
// click on that beat of every bar, which is what gives a downbeat something
// to be found by; without one, all four beats of a bar are indistinguishable.
void appendClicks(std::vector<float>& env, float seconds, float bpm,
    float firstClickAt = 0.f, int32_t accentBeat = -1)
{
    const float period = 60.f / bpm;
    const size_t count = (size_t)(seconds * EnvelopeRate);
    for(size_t i = 0; i < count; i += 1) {
        const float t = ((float)i / EnvelopeRate) - firstClickAt;
        if(t < 0.f) {
            env.emplace_back(0.f);
            continue;
        }
        const int32_t beat = (int32_t)std::floor(t / period);
        const float since = t - ((float)beat * period);
        const float gain = (accentBeat >= 0 && beat % BeatsPerBar == accentBeat) ? 1.8f : 0.9f;
        env.emplace_back(gain * std::exp(-40.f * since));
    }
}

void appendSilence(std::vector<float>& env, float seconds)
{
    env.insert(env.end(), (size_t)(seconds * EnvelopeRate), 0.f);
}

float seconds(const std::vector<float>& env)
{
    return (float)env.size() / EnvelopeRate;
}

// Distance between two phases on a circle one bar round, since a downbeat
// reported a whole bar later is the same downbeat.
float phaseError(float got, float want, float bar)
{
    float d = std::fmod(got - want, bar);
    if(d < 0.f) d += bar;
    // abs only to turn fmod's negative zero into a zero that prints as one.
    return std::abs(std::min(d, bar - d));
}

void checkTempoAndDownbeat(const char* name, const std::vector<float>& env,
    float bpm, float expectedDownbeat)
{
    const auto r = OFS_Tempo::Detect(env, EnvelopeRate, 0.f, seconds(env));
    const float bar = (60.f / bpm) * (float)BeatsPerBar;
    const float err = phaseError(r.measureOffsetSeconds, expectedDownbeat, bar);
    const bool ok = r.valid
        && std::abs(r.bpm - bpm) <= BpmTolerance
        && err <= PhaseToleranceSeconds;
    expect(ok, name, "bpm %.3f  downbeat %.3fs (want %.3fs, out %.3fs)",
        r.bpm, r.measureOffsetSeconds, expectedDownbeat, err);
}

}

int main()
{
    std::printf("tempo and downbeat, every beat equally loud\n");
    // The first beat sits on the very first sample. This is the case that
    // used to come back one to three beats late: the novelty differencing
    // wiped out the onset at sample zero, so the downbeat candidate holding it
    // could never win.
    for(float length : { 30.f, 59.f, 61.f, 70.f, 120.f }) {
        std::vector<float> env;
        appendClicks(env, length, 120.f);
        char name[64];
        std::snprintf(name, sizeof(name), "120 BPM, %.0fs, first beat at 0", length);
        checkTempoAndDownbeat(name, env, 120.f, 0.f);
    }
    for(float bpm : { 90.f, 128.f, 140.f }) {
        std::vector<float> env;
        appendClicks(env, 70.f, bpm);
        char name[64];
        std::snprintf(name, sizeof(name), "%.0f BPM, 70s, first beat at 0", bpm);
        checkTempoAndDownbeat(name, env, bpm, 0.f);
    }

    std::printf("\ndownbeat when the track does not start at zero\n");
    for(float offset : { 0.25f, 1.f, 2.f }) {
        std::vector<float> env;
        appendClicks(env, 70.f, 120.f, offset);
        char name[64];
        std::snprintf(name, sizeof(name), "120 BPM, first beat at %.2fs", offset);
        checkTempoAndDownbeat(name, env, 120.f, offset);
    }

    std::printf("\ndownbeat follows a real accent\n");
    // The detector prefers the first beat when nothing separates them. These
    // make sure that preference gives way to evidence, on every beat of the bar.
    for(int32_t accent = 0; accent < BeatsPerBar; accent += 1) {
        std::vector<float> env;
        appendClicks(env, 70.f, 120.f, 0.f, accent);
        char name[64];
        std::snprintf(name, sizeof(name), "120 BPM, accent on beat %d", accent);
        checkTempoAndDownbeat(name, env, 120.f, (float)accent * (60.f / 120.f));
    }

    std::printf("\nno music\n");
    {
        std::vector<float> env;
        appendSilence(env, 60.f);
        const auto r = OFS_Tempo::Detect(env, EnvelopeRate, 0.f, seconds(env));
        expect(!r.valid || r.confidence < 0.35f, "60s of silence reads as no tempo",
            "valid %d  confidence %.2f", (int)r.valid, r.confidence);
    }

    std::printf("\ntrack segmentation\n");
    {
        // The layout of the test clip used by hand: 120 BPM, a gap too short to
        // count as a break of its own, then 90 BPM.
        std::vector<float> env;
        appendClicks(env, 60.f, 120.f);
        appendSilence(env, 10.f);
        appendClicks(env, 80.f, 90.f);
        const auto segments = OFS_Tempo::DetectSegments(env, EnvelopeRate);

        std::vector<const OFS_Tempo::TempoSegment*> music;
        for(const auto& s : segments) {
            if(s.hasMusic) music.emplace_back(&s);
        }
        expect(music.size() == 2, "two tracks found", "%d musical of %d segments",
            (int)music.size(), (int)segments.size());
        if(music.size() == 2) {
            expect(std::abs(music[0]->tempo.bpm - 120.f) <= 1.f, "first track at 120 BPM",
                "%.3f", music[0]->tempo.bpm);
            expect(std::abs(music[1]->tempo.bpm - 90.f) <= 1.f, "second track at 90 BPM",
                "%.3f", music[1]->tempo.bpm);
            // Anywhere across the gap is a fair place for the boundary.
            const float boundary = music[1]->startTime;
            expect(boundary >= 58.f && boundary <= 72.f, "boundary falls in the gap",
                "second track starts at %.2fs", boundary);
        }
    }

    std::printf("\n%d of %d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
