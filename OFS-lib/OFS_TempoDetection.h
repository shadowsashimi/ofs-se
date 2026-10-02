#pragma once

// ============================================================================
//  Tempo detection from the audio waveform envelope.
//
//  OFS already decodes the audio into a downsampled amplitude envelope for the
//  timeline waveform (see OFS_Waveform). That envelope is effectively an onset
//  strength signal at ~160 Hz, which is all a tempo estimator needs, so tempo
//  and track segmentation can be derived offline with no extra dependencies
//  and no second decode.
// ============================================================================

#include <vector>
#include <cstdint>

namespace OFS_Tempo
{

struct TempoResult
{
    float bpm = 0.f;
    // Absolute time of a beat, in seconds from the start of the media.
    float beatOffsetSeconds = 0.f;
    // Absolute time of a downbeat, that is the first beat of a bar. The tempo
    // grid measures in whole bars by default, so aligning to a beat alone
    // leaves it correct in spacing but off by up to three beats in phase.
    float measureOffsetSeconds = 0.f;
    // The stretch actually measured, after a non-musical intro and outro were
    // trimmed off. Equal to the range asked for when nothing was trimmed, so
    // the UI can say what was skipped by comparing the two.
    float analysedStartSeconds = 0.f;
    float analysedEndSeconds = 0.f;
    // Autocorrelation peak height relative to the mean, squashed to 0..1.
    // Below ~0.35 the estimate is usually meaningless (speech, ambient, silence).
    float confidence = 0.f;
    bool valid = false;
};

struct TempoSegment
{
    float startTime = 0.f;
    float endTime = 0.f;
    TempoResult tempo;
    // False for the stretches between the music: an intro, an outro, or a
    // spoken passage with nothing steady under it. These carry no usable tempo,
    // so they are reported to show the shape of the media rather than to be
    // turned into chapters the grid would follow.
    bool hasMusic = false;
};

struct DetectSettings
{
    float minBpm = 60.f;
    float maxBpm = 200.f;
    // Tempo estimation is ambiguous by factors of two. Estimates are pulled
    // toward this range when a half/double candidate scores comparably.
    float preferredBpmCenter = 120.f;
};

// Estimates the tempo over [startTime, endTime) of the envelope.
TempoResult Detect(const std::vector<float>& envelope, float envelopeRate,
    float startTime, float endTime,
    const DetectSettings& settings = DetectSettings()) noexcept;

// Splits the envelope into the stretches that carry music, and the breaks
// between them. A stretch is split further wherever the tempo genuinely
// changes, which for a mix or a compilation corresponds to individual tracks.
// Returns segments in order covering the whole media, each musical one
// re-analysed over its final bounds and flagged with hasMusic.
std::vector<TempoSegment> DetectSegments(const std::vector<float>& envelope, float envelopeRate,
    float minSegmentSeconds = 30.f,
    const DetectSettings& settings = DetectSettings()) noexcept;

}
