#include "OFS_TempoDetection.h"
#include "OFS_Profiling.h"

#include <algorithm>
#include <cmath>

namespace OFS_Tempo
{

// Half-wave rectified difference of the envelope, with a local mean removed.
// Rising edges are what carry the beat; subtracting a running mean keeps a
// loud section from dominating a quiet one inside the same window.
static std::vector<float> buildNovelty(const std::vector<float>& envelope,
    int32_t from, int32_t to, float envelopeRate) noexcept
{
    std::vector<float> novelty;
    if(to - from < 2) return novelty;

    novelty.reserve(to - from);
    for(int32_t i = from; i < to; i += 1) {
        // Silence before the file starts, rather than the sample itself. Using
        // envelope[0] as its own predecessor made the difference zero, so a
        // track whose first beat lands on the first sample had that beat wiped
        // out of the novelty entirely.
        const float prev = i > 0 ? envelope[i - 1] : 0.f;
        novelty.emplace_back(std::max(0.f, envelope[i] - prev));
    }

    // ~0.3s running mean, wide enough to span a beat without smearing the bar.
    const int32_t meanWindow = std::max(3, (int32_t)(envelopeRate * 0.3f));
    std::vector<float> smoothed(novelty.size(), 0.f);
    float runningSum = 0.f;
    for(int32_t i = 0; i < (int32_t)novelty.size(); i += 1) {
        runningSum += novelty[i];
        if(i >= meanWindow) runningSum -= novelty[i - meanWindow];
        // Divided by the whole window even before it has filled, which counts
        // the samples before the start as the silence they are. Dividing by
        // the count seen so far makes the first sample its own mean, and
        // subtracting that cancels it to nothing -- the second way the first
        // onset in the range was being destroyed.
        smoothed[i] = runningSum / (float)meanWindow;
    }
    for(int32_t i = 0; i < (int32_t)novelty.size(); i += 1) {
        novelty[i] = std::max(0.f, novelty[i] - smoothed[i]);
    }
    return novelty;
}

// Unbiased autocorrelation at one lag.
static float autocorrelation(const std::vector<float>& novelty, int32_t lag) noexcept
{
    const int32_t n = (int32_t)novelty.size() - lag;
    if(n <= 0) return 0.f;
    float sum = 0.f;
    for(int32_t i = 0; i < n; i += 1) {
        sum += novelty[i] * novelty[i + lag];
    }
    return sum / (float)n;
}

// Sums the novelty in a small window around a position. Real onsets are a few
// samples wide and drift slightly, so scoring a single sample makes the search
// brittle: a phase one sample out can score near zero.
static float scoreAt(const std::vector<float>& novelty, float at, int32_t radius) noexcept
{
    const int32_t center = (int32_t)(at + 0.5f);
    float sum = 0.f;
    for(int32_t d = -radius; d <= radius; d += 1) {
        const int32_t i = center + d;
        if(i < 0 || i >= (int32_t)novelty.size()) continue;
        // Triangular weighting keeps the peak of the window on the beat.
        const float falloff = 1.f - (std::abs((float)d) / (float)(radius + 1));
        sum += novelty[i] * falloff;
    }
    return sum;
}

// Phase is only searched over a bounded span. Even a small error in the period
// accumulates: predicted beats walk off the real ones at (bars * 4 * period *
// relativeError), so at a realistic 0.7% period error anything past roughly
// eight bars is voting on beats that have already drifted. Short enough to stay
// coherent, long enough to average out a noisy onset or two.
constexpr float MaxPhaseSearchBars = 8.f;

// beatPeriod is what bounds the search span and sets the tolerance window, and
// it is not always the step: the downbeat search steps a whole bar at a time
// while still wanting a beat's tolerance and the same eight bar span. Passing
// them separately keeps "eight bars" meaning eight bars in both callers, where
// deriving the span from the step alone made the downbeat search cover
// thirty-two and widened its window to a whole beat either side.
static float totalScoreForPhase(const std::vector<float>& novelty, float phase,
    float step, float beatPeriod) noexcept
{
    // A window wide enough to tolerate onsets that are not perfectly on the
    // grid, which is all real playing.
    const int32_t radius = std::max(2, (int32_t)(beatPeriod * 0.05f));
    const float limit = std::min((float)novelty.size(), phase + (beatPeriod * 4.f * MaxPhaseSearchBars));
    float score = 0.f;
    for(float at = phase; at < limit; at += step) {
        score += scoreAt(novelty, at, radius);
    }
    return score;
}

// Finds the beat phase that best lines up with the novelty peaks.
static float findBeatPhase(const std::vector<float>& novelty, float period) noexcept
{
    if(period < 1.f || novelty.empty()) return 0.f;

    const int32_t steps = std::max(1, (int32_t)(period + 0.5f));
    float bestPhase = 0.f;
    float bestScore = -1.f;
    for(int32_t s = 0; s < steps; s += 1) {
        const float score = totalScoreForPhase(novelty, (float)s, period, period);
        if(score > bestScore) {
            bestScore = score;
            bestPhase = (float)s;
        }
    }
    return bestPhase;
}

// Picks which of the four beats in a bar is the downbeat, by scoring each
// candidate at one bar intervals. Downbeats usually carry the kick, so they
// accumulate more onset energy than the other three.
static float findMeasurePhase(const std::vector<float>& novelty, float beatPhase,
    float period, int32_t beatsPerMeasure) noexcept
{
    if(period < 1.f || novelty.empty() || beatsPerMeasure < 2) return beatPhase;

    const float measurePeriod = period * (float)beatsPerMeasure;
    // The first beat holds the position unless another candidate is clearly
    // better, not merely different. Where nothing separates the beats of a bar
    // -- four on the floor, a click track, a loop with no accent -- the four
    // scores come out level and picking the highest is picking noise, which
    // lands the bar lines on beat two or three for no reason anyone can hear.
    // Music starts on a downbeat far more often than not, so that is where a
    // tie goes.
    constexpr float ClearlyBetter = 1.05f;
    float bestPhase = beatPhase;
    float bestScore = totalScoreForPhase(novelty, beatPhase, measurePeriod, period);
    for(int32_t k = 1; k < beatsPerMeasure; k += 1) {
        const float phase = beatPhase + (period * (float)k);
        const float score = totalScoreForPhase(novelty, phase, measurePeriod, period);
        if(score > bestScore * ClearlyBetter) {
            bestScore = score;
            bestPhase = phase;
        }
    }
    return bestPhase;
}

// Best phase-aligned onset energy across the whole range, for one period.
static float alignmentScore(const std::vector<float>& novelty, float period, int32_t radius) noexcept
{
    const float length = (float)novelty.size();
    float best = 0.f;
    for(float phase = 0.f; phase < period; phase += 1.f) {
        float score = 0.f;
        for(float at = phase; at < length; at += period) {
            score += scoreAt(novelty, at, radius);
        }
        best = std::max(best, score);
    }
    return best;
}

// The autocorrelation peak is a good coarse period but only weakly sensitive to
// a small error in it: a period a fraction of a percent out still correlates
// almost as well, and then walks the grid a whole beat off over a couple of
// minutes. Reported as a downbeat that starts right and drifts away.
//
// Alignment across the whole range is sensitive to exactly that, because a
// wrong period cannot keep one phase fitting from end to end. Measured against
// hand marked stages this took the worst tempo error from 1.66% to 0.10% and
// halved the total drift.
constexpr float MinSecondsForPeriodRefine = 30.f;
// Wide enough to reach the answer. One marked stage's autocorrelation peak sits
// 1.76% away from where alignment puts it, so a window of 1.5% could not get
// there and left that stage 31ms out where the other two were within 16. At
// 2.5% all three land within 16ms of a hand marked beat, and all three agree on
// the tempo to within 0.02 BPM, which they did not before.
constexpr float PeriodSearchRange = 0.025f;

static float refinePeriod(const std::vector<float>& novelty, float period, float envelopeRate) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    if(period < 4.f) return period;
    if((float)novelty.size() / envelopeRate < MinSecondsForPeriodRefine) return period;

    // Tighter than the phase search uses. A wide window forgives the very error
    // being looked for.
    const int32_t radius = std::max(1, (int32_t)(period * 0.02f));

    float best = period;
    float bestScore = -1.f;
    const float coarseStep = std::max(0.05f, period * 0.0015f);
    for(float p = period * (1.f - PeriodSearchRange); p <= period * (1.f + PeriodSearchRange); p += coarseStep) {
        const float score = alignmentScore(novelty, p, radius);
        if(score > bestScore) {
            bestScore = score;
            best = p;
        }
    }

    const float fineStep = std::max(0.01f, period * 0.0002f);
    const float lo = best - (period * 0.002f);
    const float hi = best + (period * 0.002f);
    for(float p = lo; p <= hi; p += fineStep) {
        const float score = alignmentScore(novelty, p, radius);
        if(score > bestScore) {
            bestScore = score;
            best = p;
        }
    }
    return best;
}

// One estimation pass over [from, to). Phases are returned in samples relative
// to `from`, because the caller may run this twice over different ranges.
struct Estimate
{
    float period = 0.f;
    float beatPhase = 0.f;
    float measurePhase = 0.f;
    float peakOverMean = 0.f;
    bool valid = false;
};

static Estimate estimateOver(const std::vector<float>& envelope, float envelopeRate,
    int32_t from, int32_t to, const DetectSettings& settings) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    Estimate result;
    if(to - from < 2) return result;

    const int32_t lagMin = std::max(1, (int32_t)(60.f * envelopeRate / settings.maxBpm));
    const int32_t lagMax = (int32_t)(60.f * envelopeRate / settings.minBpm);
    // Needs a couple of periods of the slowest tempo to correlate against.
    if(to - from < lagMax * 2) return result;

    auto novelty = buildNovelty(envelope, from, to, envelopeRate);
    if(novelty.empty()) return result;

    std::vector<float> acf(lagMax + 1, 0.f);
    float acfSum = 0.f;
    int32_t acfCount = 0;
    int32_t bestLag = 0;
    float bestValue = 0.f;
    for(int32_t lag = lagMin; lag <= lagMax; lag += 1) {
        const float value = autocorrelation(novelty, lag);
        acf[lag] = value;
        acfSum += value;
        acfCount += 1;
        if(value > bestValue) {
            bestValue = value;
            bestLag = lag;
        }
    }
    if(bestLag == 0 || bestValue <= 0.f || acfCount == 0) return result;

    // Resolve the octave ambiguity: half and double tempo correlate almost as
    // well, so score each candidate against a preference for the middle of the
    // range before committing.
    auto scoreFor = [&](int32_t lag) noexcept -> float {
        if(lag < lagMin || lag > lagMax) return -1.f;
        const float bpm = 60.f * envelopeRate / (float)lag;
        const float octavesAway = std::log2(bpm / settings.preferredBpmCenter);
        const float spread = 0.75f;
        const float preference = std::exp(-0.5f * (octavesAway / spread) * (octavesAway / spread));
        return acf[lag] * preference;
    };

    int32_t chosenLag = bestLag;
    float chosenScore = scoreFor(bestLag);
    const float octaveFactors[2] = { 0.5f, 2.f };
    for(int32_t f = 0; f < 2; f += 1) {
        const int32_t candidate = (int32_t)std::lround(bestLag * octaveFactors[f]);
        const float score = scoreFor(candidate);
        if(score > chosenScore) {
            chosenScore = score;
            chosenLag = candidate;
        }
    }

    const float mean = acfSum / (float)acfCount;
    const float peakOverMean = mean > 0.f ? (acf[chosenLag] / mean) : 0.f;

    // The autocorrelation is only sampled at whole sample lags, which caps bpm
    // resolution at roughly 1% in this range. Interpolating a parabola through
    // the peak and its neighbours recovers a fractional lag, and that matters
    // for more than tidiness: phase is searched over many bars, so a period
    // that is slightly wrong walks the predicted beats off the real ones.
    float period = (float)chosenLag;
    if(chosenLag > lagMin && chosenLag < lagMax) {
        const float prev = acf[chosenLag - 1];
        const float here = acf[chosenLag];
        const float next = acf[chosenLag + 1];
        const float denom = prev - (2.f * here) + next;
        if(std::abs(denom) > 1e-12f) {
            const float shift = 0.5f * (prev - next) / denom;
            // A parabola fitted to a genuine peak puts the vertex inside the
            // sample either side of it; anything further means the fit is not
            // describing a peak and is better ignored.
            if(shift > -1.f && shift < 1.f) period += shift;
        }
    }

    // Refine before anything downstream uses it: the phase search, the
    // downbeat search and everything the caller does with the result all
    // inherit whatever error is left in the period.
    period = refinePeriod(novelty, period, envelopeRate);

    result.period = period;
    result.beatPhase = findBeatPhase(novelty, period);
    result.measurePhase = findMeasurePhase(novelty, result.beatPhase, period, 4);
    result.peakOverMean = peakOverMean;
    result.valid = true;
    return result;
}

// How much better the best phase is than an average one, over one window. A
// steady grid of onsets makes one phase score far above the rest; speech,
// effects and room noise spread the energy evenly and score near zero. This is
// what separates music from an intro without needing to know anything about
// either.
static float beatClarity(const std::vector<float>& novelty,
    float windowStart, float windowEnd, float period) noexcept
{
    const int32_t radius = std::max(2, (int32_t)(period * 0.05f));
    const int32_t phaseCount = std::max(1, (int32_t)(period + 0.5f));
    float best = 0.f;
    float sum = 0.f;
    for(int32_t p = 0; p < phaseCount; p += 1) {
        float score = 0.f;
        for(float at = windowStart + (float)p; at < windowEnd; at += period) {
            score += scoreAt(novelty, at, radius);
        }
        sum += score;
        best = std::max(best, score);
    }
    const float mean = sum / (float)phaseCount;
    return mean > 0.f ? ((best - mean) / mean) : 0.f;
}

// A window whose best phase scores this much better than an average one has a
// beat in it.
//
// This used to be a share of the clearest window in the range as well, which
// scaled the bar by the loudest thing in the file: a video containing one very
// dense passage judged all of its quieter music against that passage and called
// it silence. On a second video, whose peak clarity is 8.87 against the first
// video's 5.21, that put the bar at 3.10 and carved a hundred and fifty seconds
// of false break out of the middle of a single song.
//
// A fixed bar has no such coupling. At 1.5 the first video's stages come out
// exactly as they did before, measured against its marked boundaries.
constexpr float MinBeatClarity = 1.5f;
// Trimming is only worth doing on a range long enough to hold an intro. It also
// keeps the per window cost out of DetectSegments, which analyses short spans.
constexpr float MinSecondsForTrim = 60.f;

// Narrows [from, to) to the stretch that actually carries a beat, dropping a
// non-musical intro and outro. Returns false when there is nothing to trim, or
// nothing musical to find.
static bool trimToMusical(const std::vector<float>& envelope, float envelopeRate,
    int32_t from, int32_t to, float period, int32_t& outFrom, int32_t& outTo) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    outFrom = from;
    outTo = to;
    if(period < 1.f) return false;
    if((float)(to - from) / envelopeRate < MinSecondsForTrim) return false;

    auto novelty = buildNovelty(envelope, from, to, envelopeRate);
    if(novelty.empty()) return false;

    const float bar = period * 4.f;
    const float window = bar * 8.f;
    const float hop = bar * 2.f;
    const float length = (float)novelty.size();

    std::vector<float> starts;
    std::vector<float> clarity;
    for(float start = 0.f; start + (bar * 2.f) < length; start += hop) {
        starts.emplace_back(start);
        clarity.emplace_back(beatClarity(novelty, start, std::min(length, start + window), period));
    }
    if(starts.empty()) return false;

    const float threshold = MinBeatClarity;

    int32_t firstIdx = -1;
    int32_t lastIdx = -1;
    for(int32_t i = 0; i < (int32_t)clarity.size(); i += 1) {
        if(clarity[i] < threshold) continue;
        if(firstIdx < 0) firstIdx = i;
        lastIdx = i;
    }
    // Nothing in the range reads as music. Better to analyse it all and report
    // the low confidence than to invent a window.
    if(firstIdx < 0) return false;

    const int32_t trimmedFrom = from + (int32_t)starts[firstIdx];
    const int32_t trimmedTo = std::min(to, from + (int32_t)(starts[lastIdx] + window));
    if(trimmedTo - trimmedFrom < 2) return false;
    if(trimmedFrom == from && trimmedTo == to) return false;

    outFrom = trimmedFrom;
    outTo = trimmedTo;
    return true;
}

TempoResult Detect(const std::vector<float>& envelope, float envelopeRate,
    float startTime, float endTime, const DetectSettings& settings) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    TempoResult result;
    if(envelope.empty() || envelopeRate <= 0.f || endTime <= startTime) return result;

    const int32_t from = std::max(0, (int32_t)(startTime * envelopeRate));
    const int32_t to = std::min((int32_t)envelope.size(), (int32_t)(endTime * envelopeRate));
    if(to - from < 2) return result;

    // First pass over everything asked for, which is enough to get a period.
    // The period is what makes it possible to ask where the beat actually is.
    auto estimate = estimateOver(envelope, envelopeRate, from, to, settings);
    if(!estimate.valid) return result;

    // Then again over just the musical stretch. Phase was the reason for this:
    // it is searched from the start of the range and scored over the first
    // eight bars, so a range opening on an intro anchored the whole grid to
    // whatever effect happened to be loudest there. Re-estimating also sharpens
    // the tempo, which the non-musical material was pulling around.
    int32_t usedFrom = from;
    int32_t usedTo = to;
    int32_t musicalFrom = from;
    int32_t musicalTo = to;
    if(trimToMusical(envelope, envelopeRate, from, to, estimate.period, musicalFrom, musicalTo)) {
        auto trimmed = estimateOver(envelope, envelopeRate, musicalFrom, musicalTo, settings);
        if(trimmed.valid) {
            estimate = trimmed;
            usedFrom = musicalFrom;
            usedTo = musicalTo;
        }
    }

    result.bpm = 60.f * envelopeRate / estimate.period;
    result.beatOffsetSeconds = ((float)usedFrom + estimate.beatPhase) / envelopeRate;
    result.measureOffsetSeconds = ((float)usedFrom + estimate.measurePhase) / envelopeRate;
    result.analysedStartSeconds = (float)usedFrom / envelopeRate;
    result.analysedEndSeconds = (float)usedTo / envelopeRate;
    // peakOverMean sits near 1 for noise and climbs past 2 for a clear beat.
    result.confidence = std::min(1.f, std::max(0.f, (estimate.peakOverMean - 1.f) / 2.f));
    result.valid = true;
    return result;
}

// One tempo gets reported at several related rates, and none of them mean a new
// track. Half and double come from the octave ambiguity every autocorrelation
// has. The thirds come from a dotted or triplet pulse, which is what the
// estimator locks onto where the backbeat thins out, as it does under a spoken
// section with the beat carrying on quietly behind.
//
// Both were measured on a single 128.6 BPM song: it read 64.4 across a half
// time passage and 85.7 under speech, each with full confidence, and each was
// taken for a track boundary. That one song came out as three.
static bool sameTempoFamily(float a, float b, float tolerance) noexcept
{
    if(a <= 0.f || b <= 0.f) return false;
    const float factors[9] = {
        0.25f, 1.f / 3.f, 0.5f, 2.f / 3.f, 1.f, 1.5f, 2.f, 3.f, 4.f
    };
    for(int32_t f = 0; f < 9; f += 1) {
        if(std::abs(a - (b * factors[f])) / a <= tolerance) return true;
    }
    return false;
}

// A dip shorter than this inside a musical stretch is a passage, not a break.
//
// Set from hand marked stage boundaries rather than guessed. On that material
// the quiet passages within a stage run 26 to 30 seconds, and the real gaps
// between stages are 52 and 54, so anything in between separates the stages
// while keeping each one whole. Twenty was too short and broke every stage into
// pieces; at forty two stages merged into one.
constexpr float MaxDipSeconds = 35.f;

// A stretch has to measure at least this well to count as music.
//
// This was 0.6, which rejected correct answers by construction. Confidence is
// not a measure of whether something is music; it is a measure of how evenly
// the beat is spread across what was measured. A whole stage necessarily
// includes its own quiet passages and so scores lower than a dense window
// picked out of the middle of it. The three hand marked stages measure 0.33,
// 0.52 and 0.39, so the gate has to sit below 0.33 for a correctly sized stage
// to pass at all. At 0.30 nothing outside a marked stage was promoted to music.
constexpr float MinTrackConfidence = 0.30f;

// A quiet lead in reads as part of the run, because clarity only asks whether
// the onsets line up, and a sparse groove lines up as well as a loud one. Where
// a track begins for scripting purposes is where the level arrives, which is
// not the same place: measured on real material the beat became readable at
// 45s and the music landed at 57s, twelve seconds later.
//
// So the start is moved forward to where the level first reaches a fair share
// of what the run runs at, and holds there.
constexpr float StartLevelFraction = 0.6f;
constexpr int32_t StartLevelHoldSeconds = 3;

static int32_t refineRunStart(const std::vector<float>& envelope, float envelopeRate,
    int32_t from, int32_t to) noexcept
{
    std::vector<float> level;
    for(int32_t start = from; start < to; start += (int32_t)envelopeRate) {
        const int32_t end = std::min(to, start + (int32_t)envelopeRate);
        if(end <= start) break;
        float sum = 0.f;
        for(int32_t i = start; i < end; i += 1) sum += envelope[i];
        level.emplace_back(sum / (float)(end - start));
    }
    if((int32_t)level.size() < StartLevelHoldSeconds * 2) return from;

    std::vector<float> sorted = level;
    std::sort(sorted.begin(), sorted.end());
    const float threshold = StartLevelFraction * sorted[sorted.size() / 2];

    // Never move more than halfway in. The aim is to drop a quiet lead in, not
    // to decide the track starts somewhere else entirely.
    const int32_t limit = (int32_t)level.size() / 2;
    for(int32_t s = 0; s < limit; s += 1) {
        bool held = true;
        for(int32_t k = 0; k < StartLevelHoldSeconds; k += 1) {
            const int32_t at = s + k;
            if(at < (int32_t)level.size() && level[at] < threshold) {
                held = false;
                break;
            }
        }
        if(held) return from + (int32_t)((float)s * envelopeRate);
    }
    return from;
}

// The mirror of the start refinement, for the other end. Clarity loses a track
// before it stops: the beat thins out while the music is still playing, so the
// run ends early and the last stretch of a stage is cut off.
//
// Unlike the start, this has to ride over a dip. On the material this was
// measured against, one second at 536s reads below the bar and the loudest
// passage of the whole stage runs from 538 to 566. Stopping at the first
// second under the bar ended the stage thirty one seconds early, immediately
// before its climax.
constexpr int32_t EndDipToleranceSeconds = 5;
constexpr float MaxEndExtensionSeconds = 180.f;

static int32_t refineRunEnd(const std::vector<float>& envelope, float envelopeRate,
    int32_t from, int32_t to) noexcept
{
    const int32_t second = std::max(1, (int32_t)envelopeRate);

    std::vector<float> level;
    for(int32_t start = from; start < to; start += second) {
        const int32_t end = std::min(to, start + second);
        if(end <= start) break;
        float sum = 0.f;
        for(int32_t i = start; i < end; i += 1) sum += envelope[i];
        level.emplace_back(sum / (float)(end - start));
    }
    if((int32_t)level.size() < StartLevelHoldSeconds * 2) return to;

    std::vector<float> sorted = level;
    std::sort(sorted.begin(), sorted.end());
    const float threshold = StartLevelFraction * sorted[sorted.size() / 2];

    auto holdsFrom = [&](int32_t at) noexcept {
        for(int32_t k = 0; k < StartLevelHoldSeconds; k += 1) {
            const int32_t a = at + (k * second);
            const int32_t b = std::min((int32_t)envelope.size(), a + second);
            if(b <= a) return false;
            float sum = 0.f;
            for(int32_t i = a; i < b; i += 1) sum += envelope[i];
            if((sum / (float)(b - a)) < threshold) return false;
        }
        return true;
    };

    const int32_t limit = std::min((int32_t)envelope.size(),
        to + (int32_t)(MaxEndExtensionSeconds * envelopeRate));
    int32_t end = to;
    int32_t misses = 0;
    for(int32_t at = to; at + second <= limit; at += second) {
        if(holdsFrom(at)) {
            end = std::min(limit, at + second);
            misses = 0;
        }
        else {
            misses += 1;
            if(misses > EndDipToleranceSeconds) break;
        }
    }
    return end;
}

// The stretches that carry a beat, as sample ranges. The same clarity measure
// the trimming uses, applied across the whole media rather than to its ends.
static std::vector<std::pair<int32_t, int32_t>> findMusicalRuns(
    const std::vector<float>& envelope, float envelopeRate,
    int32_t from, int32_t to, float period, float minRunSeconds) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<std::pair<int32_t, int32_t>> runs;
    if(period < 1.f) return runs;

    auto novelty = buildNovelty(envelope, from, to, envelopeRate);
    if(novelty.empty()) return runs;

    const float bar = period * 4.f;
    const float window = bar * 8.f;
    const float hop = bar * 2.f;
    const float length = (float)novelty.size();

    std::vector<float> starts;
    std::vector<float> clarity;
    for(float start = 0.f; start + (bar * 2.f) < length; start += hop) {
        starts.emplace_back(start);
        clarity.emplace_back(beatClarity(novelty, start, std::min(length, start + window), period));
    }
    if(starts.empty()) return runs;

    const float threshold = MinBeatClarity;

    int32_t runStart = -1;
    for(int32_t i = 0; i < (int32_t)clarity.size(); i += 1) {
        const bool musical = clarity[i] >= threshold;
        if(musical && runStart < 0) runStart = i;
        if(!musical && runStart >= 0) {
            runs.emplace_back(from + (int32_t)starts[runStart],
                std::min(to, from + (int32_t)(starts[i - 1] + window)));
            runStart = -1;
        }
    }
    if(runStart >= 0) {
        runs.emplace_back(from + (int32_t)starts[runStart],
            std::min(to, from + (int32_t)(starts.back() + window)));
    }

    const int32_t maxDip = (int32_t)(MaxDipSeconds * envelopeRate);
    std::vector<std::pair<int32_t, int32_t>> merged;
    for(size_t i = 0; i < runs.size(); i += 1) {
        if(!merged.empty() && (runs[i].first - merged.back().second) <= maxDip) {
            merged.back().second = runs[i].second;
        }
        else {
            merged.emplace_back(runs[i]);
        }
    }

    // Refine before the length test, since a start that moves can take a run
    // under the minimum, and the reference level wants the whole run to
    // average over.
    for(size_t i = 0; i < merged.size(); i += 1) {
        merged[i].first = refineRunStart(envelope, envelopeRate, merged[i].first, merged[i].second);
        merged[i].second = refineRunEnd(envelope, envelopeRate, merged[i].first, merged[i].second);
    }

    const int32_t minRun = (int32_t)(minRunSeconds * envelopeRate);
    std::vector<std::pair<int32_t, int32_t>> kept;
    for(size_t i = 0; i < merged.size(); i += 1) {
        if(merged[i].second - merged[i].first >= minRun) kept.emplace_back(merged[i]);
    }
    return kept;
}

// Times inside [from, to) at which the tempo changes, for splitting one
// unbroken stretch of music into the tracks it is made of.
static std::vector<float> tempoChangesWithin(const std::vector<float>& envelope, float envelopeRate,
    float from, float to, const DetectSettings& settings) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<float> boundaries;
    const float WindowSeconds = 20.f;
    const float HopSeconds = 5.f;
    if(to - from < WindowSeconds * 2.f) return boundaries;

    struct Window { float at; TempoResult tempo; };
    std::vector<Window> windows;
    for(float at = from; at + WindowSeconds <= to; at += HopSeconds) {
        Window w;
        w.at = at;
        w.tempo = Detect(envelope, envelopeRate, at, at + WindowSeconds, settings);
        windows.emplace_back(w);
    }
    if(windows.empty()) return boundaries;

    const float BpmTolerance = 0.03f; // 3%
    const float MinConfidence = 0.35f;
    // How many consecutive confident windows must agree on a new tempo before
    // it counts as a track change. Windows are 20s on a 5s hop, so a genuine
    // change takes four of them to clear the old track completely; three is
    // already past the point where a stray reading could explain it.
    const size_t MinWindowsForChange = 3;

    auto sameTempo = [&](float a, float b) noexcept {
        return sameTempoFamily(a, b, BpmTolerance);
    };

    auto medianOf = [](std::vector<float> values) noexcept -> float {
        if(values.empty()) return 0.f;
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };

    // Windows that cannot be read carry no evidence either way, so they are
    // dropped rather than compared. This matters more than it sounds: letting
    // one unusable window both open a boundary and become the reference for
    // everything after it turned a single quiet passage into several tracks.
    std::vector<Window> usable;
    usable.reserve(windows.size());
    for(size_t i = 0; i < windows.size(); i += 1) {
        const auto& t = windows[i].tempo;
        if(t.valid && t.confidence >= MinConfidence && t.bpm > 0.f) {
            usable.emplace_back(windows[i]);
        }
    }
    if(usable.empty()) return boundaries;

    // The run is compared against its median rather than its first window, so
    // one odd reading inside a track cannot drag the reference with it.
    std::vector<float> run{ usable[0].tempo.bpm };
    std::vector<Window> pending;
    std::vector<float> pendingBpm;

    for(size_t i = 1; i < usable.size(); i += 1) {
        const float bpm = usable[i].tempo.bpm;
        if(sameTempo(medianOf(run), bpm)) {
            run.emplace_back(bpm);
            pending.clear();
            continue;
        }

        // Only treat it as a change once enough windows in a row disagree the
        // same way; a new tempo that cannot hold for three windows is a
        // passage, not a track.
        if(!pending.empty()) {
            pendingBpm.clear();
            for(size_t p = 0; p < pending.size(); p += 1) pendingBpm.emplace_back(pending[p].tempo.bpm);
            if(!sameTempo(medianOf(pendingBpm), bpm)) pending.clear();
        }
        pending.emplace_back(usable[i]);

        if(pending.size() >= MinWindowsForChange) {
            boundaries.emplace_back(pending.front().at + (WindowSeconds * 0.5f));
            run.clear();
            for(size_t p = 0; p < pending.size(); p += 1) run.emplace_back(pending[p].tempo.bpm);
            pending.clear();
        }
    }
    return boundaries;
}


// Two stretches either side of a break are one song when the break is a
// breakdown rather than a boundary. Which it is takes three questions, and the
// third is the one that actually separates them.
constexpr float SameSongTempoTolerance = 0.005f;
// As a fraction of a beat.
constexpr float SameSongPhaseTolerance = 0.15f;
// How much of the gap has to still be keeping time.
constexpr float SameSongGapBeatFraction = 1.f / 3.f;

static bool bridgesOneSong(const std::vector<float>& envelope, float envelopeRate,
    const TempoSegment& left, const TempoSegment& right, const DetectSettings& settings) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    const TempoResult& a = left.tempo;
    const TempoResult& b = right.tempo;
    if(!a.valid || !b.valid || a.bpm <= 0.f || b.bpm <= 0.f) return false;

    // Same tempo, and not merely a related one: half time on one side of a real
    // boundary and full on the other is still two songs.
    if(std::abs(a.bpm - b.bpm) / a.bpm > SameSongTempoTolerance) return false;

    // Has the beat kept counting across the gap? Phase agreement over a gap of
    // any length is not something that happens by chance, because a tempo
    // difference of even a fraction of a percent walks the phase apart. Across
    // two videos this read 0.06 of a beat where the music was continuous, and
    // 0.35 where it was not.
    const float beat = 60.f / a.bpm;
    float residual = std::fmod(b.beatOffsetSeconds - a.beatOffsetSeconds, beat);
    if(residual < 0.f) residual += beat;
    if(residual > beat * 0.5f) residual -= beat;
    if(std::abs(residual) > beat * SameSongPhaseTolerance) return false;

    // And is the gap itself still keeping time? This is what tells a breakdown
    // from a boundary, and it is decisive: the one measured breakdown held the
    // beat through 62% of its gap, where both real boundaries managed none of
    // it at all.
    const float gapStart = left.endTime;
    const float gapEnd = right.startTime;
    const float WindowSeconds = 20.f;
    const float StepSeconds = 10.f;
    int32_t total = 0;
    int32_t keeping = 0;
    for(float at = gapStart; at + WindowSeconds <= gapEnd; at += StepSeconds) {
        const auto probe = Detect(envelope, envelopeRate, at, at + WindowSeconds, settings);
        total += 1;
        if(probe.valid && probe.confidence >= 0.35f && sameTempoFamily(a.bpm, probe.bpm, 0.03f)) {
            keeping += 1;
        }
    }
    // Too short a gap to sample. The tempo and phase agreement already stand.
    if(total == 0) return true;
    return ((float)keeping / (float)total) >= SameSongGapBeatFraction;
}

std::vector<TempoSegment> DetectSegments(const std::vector<float>& envelope, float envelopeRate,
    float minSegmentSeconds, const DetectSettings& settings) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    std::vector<TempoSegment> segments;
    if(envelope.empty() || envelopeRate <= 0.f) return segments;

    const float totalDuration = (float)envelope.size() / envelopeRate;
    if(totalDuration < 40.f) return segments;

    // A period comes first, because the clarity measure is a question about a
    // period: it asks how well the onsets line up on a grid of that spacing.
    auto global = estimateOver(envelope, envelopeRate, 0, (int32_t)envelope.size(), settings);
    if(!global.valid) return segments;

    auto runs = findMusicalRuns(envelope, envelopeRate, 0, (int32_t)envelope.size(),
        global.period, minSegmentSeconds);
    if(runs.empty()) return segments;

    auto addBreak = [&](float from, float to) noexcept {
        if(to - from <= 0.f) return;
        TempoSegment seg;
        seg.startTime = from;
        seg.endTime = to;
        seg.hasMusic = false;
        segments.emplace_back(seg);
    };

    float cursor = 0.f;
    for(size_t r = 0; r < runs.size(); r += 1) {
        const float runStart = (float)runs[r].first / envelopeRate;
        const float runEnd = (float)runs[r].second / envelopeRate;
        addBreak(cursor, runStart);

        // An unbroken stretch of music can still hold more than one track, so
        // split it where the tempo changes before measuring each part.
        std::vector<float> cuts = tempoChangesWithin(envelope, envelopeRate, runStart, runEnd, settings);
        float partStart = runStart;
        for(size_t c = 0; c <= cuts.size(); c += 1) {
            const float partEnd = (c < cuts.size()) ? cuts[c] : runEnd;
            if(partEnd - partStart < minSegmentSeconds) continue;

            TempoSegment seg;
            seg.startTime = partStart;
            seg.endTime = partEnd;
            // Measured over its own bounds, so each stretch carries its own
            // downbeat. One tempo spanning the whole media looks fine where it
            // was anchored and drifts a beat off by the far end of it.
            seg.tempo = Detect(envelope, envelopeRate, partStart, partEnd, settings);
            // Clarity got us this far, but it only asks whether some phase
            // beats an average one. Whether the answer is steady enough to hand
            // to the grid is a separate question, and the measurement's own
            // confidence is what answers it.
            seg.hasMusic = seg.tempo.valid && seg.tempo.confidence >= MinTrackConfidence;
            segments.emplace_back(seg);
            partStart = partEnd;
        }
        // A final part too short to stand on its own is absorbed by the one
        // before it, so the run stays covered end to end.
        if(!segments.empty() && segments.back().startTime >= runStart
            && segments.back().endTime < runEnd) {
            segments.back().endTime = runEnd;
        }
        cursor = runEnd;
    }
    addBreak(cursor, totalDuration);

    // A stretch that measured too poorly to call music is a break, and two
    // breaks that ended up next to each other are one break.
    std::vector<TempoSegment> joined;
    for(size_t i = 0; i < segments.size(); i += 1) {
        if(!joined.empty() && !joined.back().hasMusic && !segments[i].hasMusic) {
            joined.back().endTime = segments[i].endTime;
            joined.back().tempo = TempoResult();
            continue;
        }
        joined.emplace_back(segments[i]);
    }

    // Then join back up whatever the break was only interrupting. A song with a
    // breakdown in it reads as two stretches with a hole between them, and that
    // hole is not a track boundary: the grid runs straight through it.
    bool mergedAny = true;
    while(mergedAny) {
        mergedAny = false;
        for(size_t i = 0; i + 1 < joined.size(); i += 1) {
            if(!joined[i].hasMusic) continue;

            size_t next = i + 1;
            while(next < joined.size() && !joined[next].hasMusic) next += 1;
            if(next >= joined.size()) break;
            // Only ever bridge a single break, so this cannot walk the length
            // of the media joining everything to everything.
            if(next - i > 2) continue;
            if(!bridgesOneSong(envelope, envelopeRate, joined[i], joined[next], settings)) continue;

            TempoSegment merged;
            merged.startTime = joined[i].startTime;
            merged.endTime = joined[next].endTime;
            // Measured over the whole thing, which is a longer span than either
            // half and so a better tempo than either had on its own.
            merged.tempo = Detect(envelope, envelopeRate, merged.startTime, merged.endTime, settings);
            if(!merged.tempo.valid) merged.tempo = joined[i].tempo;
            merged.hasMusic = true;

            joined.erase(joined.begin() + i, joined.begin() + next + 1);
            joined.insert(joined.begin() + i, merged);
            mergedAny = true;
            break;
        }
    }
    return joined;
}

}
