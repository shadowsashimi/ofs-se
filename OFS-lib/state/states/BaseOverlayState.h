#pragma once

#include "OFS_StateHandle.h"

struct BaseOverlayState
{
    static constexpr auto StateName = "BaseOverlayState";

    ImColor MaxSpeedColor = ImColor(0xFF, 0x9C, 0xC0, 0xFF);
    float MaxSpeedPerSecond = 400.f;
    bool ShowMaxSpeedHighlight = false;
    bool SyncLineEnable = false;
    bool SplineMode = false;
    // Snap actions placed or dragged with the mouse onto the active overlay's
    // grid. Keyboard editing already steps on that grid, so this is what makes
    // the two agree. Held here rather than per overlay because it is an editing
    // preference: the division it snaps to is the overlay's business.
    bool SnapToGrid = false;
    // Position increment to snap to, or zero for none. 25 gives quarters,
    // which is what shaping a stroke usually wants.
    int32_t SnapPositionStep = 0;

    inline static uint32_t RegisterStatic() noexcept
    {
        return OFS_AppState<BaseOverlayState>::Register(StateName);
    }

    inline static BaseOverlayState& State(uint32_t stateHandle) noexcept
    {
        return OFS_AppState<BaseOverlayState>(stateHandle).Get();
    }
};

REFL_TYPE(BaseOverlayState)
    REFL_FIELD(MaxSpeedColor)
    REFL_FIELD(MaxSpeedPerSecond)
    REFL_FIELD(ShowMaxSpeedHighlight)
    REFL_FIELD(SyncLineEnable)
    REFL_FIELD(SplineMode)
    REFL_FIELD(SnapToGrid)
    REFL_FIELD(SnapPositionStep)
REFL_END