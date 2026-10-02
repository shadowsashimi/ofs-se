#pragma once

#include "OFS_StateHandle.h"

// What counts as too fast, too close or too long a gap, for the script check
// and for the copy "Export for device" writes.
struct ScriptCheckState
{
    static constexpr auto StateName = "ScriptCheckState";

    // 0 gentle, 1 typical, 2 fast. Kept so the bar shows what was picked; the
    // numbers below are what is actually used, and can be edited freely.
    int32_t preset = 1;
    int32_t maxSpeed = 400;
    int32_t minGapMs = 50;
    int32_t longGapSeconds = 5;

    static inline ScriptCheckState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<ScriptCheckState>(stateHandle).Get();
    }
};

REFL_TYPE(ScriptCheckState)
    REFL_FIELD(preset)
    REFL_FIELD(maxSpeed)
    REFL_FIELD(minGapMs)
    REFL_FIELD(longGapSeconds)
REFL_END
