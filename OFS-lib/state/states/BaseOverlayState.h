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
    // Snap points placed or dragged with the mouse onto a beat tick, the hits
    // found in the waveform, when one is near enough. Ahead of the grid.
    bool SnapToBeats = false;
    // How the timeline colours its lines by speed: 0 as EroScripts' heatmap,
    // 1 as the original OFS, 2 muted, for long sessions, 3 one plain colour,
    // 4 the four stops below.
    int32_t SpeedColors = 0;
    float CustomSpeed0 = 0.f;
    float CustomSpeed1 = 150.f;
    float CustomSpeed2 = 300.f;
    float CustomSpeed3 = 450.f;
    ImColor CustomColor0 = IM_COL32(0xB4, 0xB8, 0xBC, 0xFF);
    ImColor CustomColor1 = IM_COL32(0x6F, 0xA8, 0xA0, 0xFF);
    ImColor CustomColor2 = IM_COL32(0xC9, 0xA5, 0x6A, 0xFF);
    ImColor CustomColor3 = IM_COL32(0xC7, 0x71, 0x6A, 0xFF);

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
    REFL_FIELD(SnapToBeats)
    REFL_FIELD(SpeedColors)
    REFL_FIELD(CustomSpeed0)
    REFL_FIELD(CustomSpeed1)
    REFL_FIELD(CustomSpeed2)
    REFL_FIELD(CustomSpeed3)
    REFL_FIELD(CustomColor0)
    REFL_FIELD(CustomColor1)
    REFL_FIELD(CustomColor2)
    REFL_FIELD(CustomColor3)
REFL_END