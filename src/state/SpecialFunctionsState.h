#pragma once

#include "OFS_StateHandle.h"

#include <string>
#include <vector>

// ATTENTION: no reordering
enum class SpecialFunctionType : int32_t
{
	RangeExtender,
	RamerDouglasPeucker,
	MultiAxis,
	BeatFill,
	Loudness,
	TotalFunctionCount
};

// Where a tool works: a stretch of media picked one of three ways.
// ATTENTION: no reordering, saved as a number
enum BeatFillScope : int32_t
{
	BeatFillChapter,
	BeatFillSelection,
	BeatFillAllMusic,
	BeatFillScopeCount
};

// One point of a saved pattern. The time is in beats from the pattern's start,
// so the same pattern fits any tempo; the position is what it was when saved.
struct PatternPoint
{
	float beat = 0.f;
	int32_t pos = 0;
};

// A passage saved to be stamped along the beat, tempo and all.
struct StrokePattern
{
	std::string name;
	// How much of the bar one repeat covers, rounded up to a whole beat when
	// saved, so repeats stay on the beat rather than drifting off it.
	float lengthBeats = 4.f;
	std::vector<PatternPoint> points;
};

// ATTENTION: no reordering, saved as a number
enum MultiAxisPattern : int32_t
{
	MultiAxisOff,
	MultiAxisFollow,
	MultiAxisAlternate,
	MultiAxisCircle,
	MultiAxisFigureEight,
	MultiAxisPatternCount
};

// How one of twist, roll or pitch moves with the stroke it is generated from.
struct MultiAxisAxisSettings
{
	int32_t pattern = MultiAxisOff;
	// How far each way from the centre, in position units.
	int32_t amount = 20;
	// Where the axis rests. 50 is level: for roll, even left and right.
	int32_t center = 50;
	// How much of the motion gathers at the bottom of the stroke, in percent.
	int32_t bottomFocus = 0;
	// Moves the motion along the stroke, in degrees of one stroke.
	int32_t phase = 0;
	bool scaleWithStroke = true;
	bool reverse = false;
};

REFL_TYPE(MultiAxisAxisSettings)
	REFL_FIELD(pattern)
	REFL_FIELD(amount)
	REFL_FIELD(center)
	REFL_FIELD(bottomFocus)
	REFL_FIELD(phase)
	REFL_FIELD(scaleWithStroke)
	REFL_FIELD(reverse)
REFL_END

struct SpecialFunctionState
{
    static constexpr auto StateName = "SpecialFunctionState";

    SpecialFunctionType selectedFunction = SpecialFunctionType::RangeExtender;

    MultiAxisAxisSettings twist;
    MultiAxisAxisSettings roll = { MultiAxisAlternate, 20, 50, 30, 0, true, false };
    MultiAxisAxisSettings pitch;

    int32_t beatFillScope = BeatFillChapter;
    // Empty is the built-in up and down stroke; otherwise the name of a saved
    // pattern, so the choice survives a pattern being added or removed.
    std::string beatFillPattern;
    // Index into the same note lengths the tempo grid offers. 2 is a point
    // every quarter note, which is a stroke every two beats.
    int32_t beatFillSpacing = 2;
    int32_t beatFillTop = 90;
    int32_t beatFillBottom = 10;
    bool beatFillStartAtBottom = true;
    bool beatFillReplace = true;

    std::vector<StrokePattern> patterns;

    // How shallow the quietest passage gets, as a percentage of its depth.
    int32_t loudnessQuietDepth = 35;
    // 0 keeps the middle of each stroke, 1 its bottom, 2 its top.
    int32_t loudnessAnchor = 0;
    // Follow the shape of a section rather than of single beats.
    bool loudnessSections = true;

    static inline SpecialFunctionState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<SpecialFunctionState>(stateHandle).Get();
    }
};

REFL_TYPE(PatternPoint)
    REFL_FIELD(beat)
    REFL_FIELD(pos)
REFL_END

REFL_TYPE(StrokePattern)
    REFL_FIELD(name)
    REFL_FIELD(lengthBeats)
    REFL_FIELD(points)
REFL_END

REFL_TYPE(SpecialFunctionState)
    REFL_FIELD(selectedFunction, serializeEnum{})
    REFL_FIELD(twist)
    REFL_FIELD(roll)
    REFL_FIELD(pitch)
    REFL_FIELD(beatFillScope)
    REFL_FIELD(beatFillPattern)
    REFL_FIELD(beatFillSpacing)
    REFL_FIELD(beatFillTop)
    REFL_FIELD(beatFillBottom)
    REFL_FIELD(beatFillStartAtBottom)
    REFL_FIELD(beatFillReplace)
    REFL_FIELD(patterns)
    REFL_FIELD(loudnessQuietDepth)
    REFL_FIELD(loudnessAnchor)
    REFL_FIELD(loudnessSections)
REFL_END
