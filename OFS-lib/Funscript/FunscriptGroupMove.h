#pragma once

#include "FunscriptAction.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>

// Moving several selected points as one piece, which is what the timeline does
// when a selected point is dragged. Kept apart from Funscript, and from the UI,
// so it can be checked on its own: tools\groupmove-probe.ps1 builds against
// this file and nothing else from the app.
//
// The group is always placed relative to where it sat when the drag began,
// never to where the previous frame left it. The offset is then exact rather
// than the sum of many small clamped steps, and pulling a drag back to where it
// started puts every point back exactly.
namespace FunscriptGroupMove
{
    struct Offset
    {
        float time = 0.f;
        int32_t pos = 0;
    };

    // Clamps a requested offset so that shifting every point in group by it
    // keeps the group's shape and disturbs nothing around it:
    //
    //  - no point passes, or lands within minGap of, a point in others (the
    //    actions that are not moving), including an unselected point sitting
    //    between two selected ones;
    //  - no point goes before zero seconds;
    //  - positions stay within 0..100, and since every point moves by the same
    //    amount the group stops at the edge instead of being squashed against it.
    //
    // Standing still is always allowed, even where points already sit closer
    // than minGap, so a clamp can never push a group somewhere it was not
    // dragged.
    inline Offset Clamp(const FunscriptArray& others, const FunscriptArray& group,
        float timeOffset, int32_t posOffset, float minGap) noexcept
    {
        Offset result;
        if(group.empty()) return result;

        float lo = -group.front().atS;
        float hi = std::numeric_limits<float>::max();
        int32_t minPos = std::numeric_limits<int32_t>::max();
        int32_t maxPos = std::numeric_limits<int32_t>::min();

        for(const auto& point : group)
        {
            // First point not earlier than this one. The one before it, if
            // any, is the nearest earlier point that is staying put.
            auto next = others.lower_bound(point);
            if(next != others.end())
                hi = std::min(hi, next->atS - minGap - point.atS);
            if(next != others.begin())
                lo = std::max(lo, std::prev(next)->atS + minGap - point.atS);

            minPos = std::min<int32_t>(minPos, point.pos);
            maxPos = std::max<int32_t>(maxPos, point.pos);
        }

        lo = std::min(lo, 0.f);
        hi = std::max(hi, 0.f);
        result.time = std::clamp(timeOffset, lo, hi);

        const int32_t posLo = std::min(-minPos, 0);
        const int32_t posHi = std::max(100 - maxPos, 0);
        result.pos = std::clamp(posOffset, posLo, posHi);
        return result;
    }

    // The group shifted by offset. A uniform shift cannot reorder points, so
    // the result comes out sorted without sorting it.
    inline FunscriptArray Apply(const FunscriptArray& group, Offset offset) noexcept
    {
        FunscriptArray moved;
        moved.reserve(group.size());
        for(auto point : group)
        {
            point.atS += offset.time;
            point.pos = (int16_t)(point.pos + offset.pos);
            moved.emplace_back_unsorted(point);
        }
        return moved;
    }
}
