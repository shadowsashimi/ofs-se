#pragma once

#include "FunscriptAction.h"

#include <cstdint>
#include <string>
#include <vector>

// Looks over a script for the things that make a device stutter, skip, or sit
// still, and lists them with a way to jump to each. The same limits are what
// "Export for device" writes a copy against.
namespace OFS_Check
{

struct Limits
{
    // Units of position per second. A stroke faster than this arrives as a
    // jerk or is simply not followed.
    float maxSpeed = 400.f;
    // Points closer together than this cannot be told apart by a device that
    // polls, and the second one is usually dropped.
    float minGapSeconds = 0.05f;
    // A stretch this long with nothing in it reads as the script having
    // stopped, which is worth knowing about while the music carries on.
    float longGapSeconds = 5.f;
};

enum class ProblemKind : int32_t
{
    TooFast,
    TooClose,
    LongGap,
    PastTheEnd,
};

struct Problem
{
    ProblemKind kind = ProblemKind::TooFast;
    // Where to send the playhead, and the two points it sits between.
    float atTime = 0.f;
    FunscriptAction from;
    FunscriptAction to;
    // Filled in for the list: the speed, the gap, whatever the number is.
    float value = 0.f;
};

// Everything wrong with the script, in time order. Duration is the media's,
// for the points past the end of it; zero means there is no media to be past.
std::vector<Problem> Run(const FunscriptArray& actions, const Limits& limits, float duration) noexcept;

// The same actions with nothing faster than the limit: each point that arrives
// too soon after the one before is pulled back towards it, which shortens the
// stroke rather than moving it in time.
FunscriptArray LimitSpeed(const FunscriptArray& actions, const Limits& limits) noexcept;

// The same actions with the ones too close together removed. Of a pair, the
// one that goes is whichever leaves the shape closer to what it was.
FunscriptArray ThinOut(const FunscriptArray& actions, const Limits& limits) noexcept;

const char* KindName(ProblemKind kind) noexcept;

}

class OFS_ScriptCheck
{
private:
    uint32_t stateHandle = 0xFFFF'FFFF;
    std::vector<OFS_Check::Problem> problems;
    // Which script the list belongs to, so it is cleared rather than left
    // pointing at times in a script that is no longer showing.
    std::string checkedScript;
    bool checkedOnce = false;

    void run() noexcept;
public:
    static constexpr const char* WindowId = "###SCRIPT_CHECK";
    OFS_ScriptCheck() noexcept;
    void ShowWindow(bool* open) noexcept;
    // For the export, which offers the same numbers.
    OFS_Check::Limits CurrentLimits() const noexcept;
    // What the last check found, for the UI test driver's state dump.
    inline int32_t ProblemCount() const noexcept { return (int32_t)problems.size(); }
    inline bool HasChecked() const noexcept { return checkedOnce; }
};
