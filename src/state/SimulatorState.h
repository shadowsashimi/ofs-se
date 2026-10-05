#pragma once

#include "OFS_StateHandle.h"


struct SimulatorState
{
    static constexpr auto StateName = "SimulatorState";

    ImVec2 P1 = {600.f, 300.f};
    ImVec2 P2 = {600.f, 700.f};
    ImColor Text = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
    ImColor Front = IM_COL32(0xE8, 0x54, 0x8A, 0xFF);   // Sashimi pink
    ImColor Back = IM_COL32(0x10, 0x10, 0x10, 0xBF);
    ImColor Border = IM_COL32(0x38, 0x38, 0x38, 0xFF);
    ImColor ExtraLines = IM_COL32(0x38, 0x38, 0x38, 0xFF);
    ImColor Indicator = IM_COL32(0xE6, 0xE6, 0xE6, 0xFF);
    // The rod of the 3D model. A skin tone to begin with.
    ImColor RodColor = IM_COL32(0xB4, 0x7B, 0x70, 0xFF);
    float Width = 120.f;
    float BorderWidth = 8.f;
    float ExtraLineWidth = 4.f;
    float LineWidth = 4.f;
    float GlobalOpacity = 0.75f;

    int32_t ExtraLinesCount = 0;

    // Replaces the flat bar with a 3D model of the stroker driven by every
    // loaded axis at once.
    bool ShowMultiAxis = false;
    // Size of that model relative to the configured bar length.
    float ModelScale = 1.f;
    // Print the live value of each mapped axis next to the model.
    bool ShowAxisReadout = false;
    // Cut away the near half of the case so the sleeve inside is visible.
    bool CutawayCase = true;
    // Draw the rod. Hidden, it still strokes and shapes the sleeve, so the
    // lips and canal can be watched deforming with nothing in the way.
    bool ShowRod = true;
    // Outline the opening where it comes round, inside the lips.
    bool ShowRim = false;
    // Mark which way the front of the case faces, so twist can be read.
    bool ShowTwistIndicator = true;
    // Camera elevation in degrees. Zero looks at the model straight on;
    // positive raises the viewpoint from below, which is what brings the
    // orifice in the underside into view.
    float CameraElevation = 0.f;
    // Camera turn around the model in degrees. Zero is straight on; turned,
    // pitch and surge, which move towards and away from a camera in front,
    // can be seen.
    float CameraYaw = 0.f;
    // Stroke (L0) limits for the 3D model, in percent of its full travel, as
    // a machine's L0 range is set: the script's 0 moves the model only down
    // to StrokeMin and its 100 only up to StrokeMax.
    int32_t StrokeMin = 0;
    int32_t StrokeMax = 100;

    bool EnableIndicators = true;
    bool EnablePosition = false;
    bool EnableHeightLines = true;
    // Locked to begin with. Unlocked, the bar's ends and middle take clicks
    // wherever it is drawn, and its default place overlaps the timeline, so a
    // new user's first attempts at placing a point moved the simulator.
    bool LockedPosition = true;
    // Keep the simulator inside the video player, sized to it, following the
    // player as its window is moved or resized. Which side it sits against,
    // 0 left, 1 centre, 2 right, and how tall, as a share of the player.
    bool FitToPlayer = false;
    int32_t FitAnchor = 2;
    float FitSize = 0.9f;

    inline static SimulatorState& State(uint32_t stateHandle) noexcept
    {
        return OFS_ProjectState<SimulatorState>(stateHandle).Get();
    }
};

struct SimulatorDefaultConfigState
{
    static constexpr auto StateName = "SimulatorDefaultConfigState";
    SimulatorState defaultState;

    inline static SimulatorDefaultConfigState& StaticStateSlow() noexcept
    {
        // This shouldn't be done in hot paths but shouldn't be a problem otherwise.
        uint32_t handle = OFS_AppState<SimulatorDefaultConfigState>::Register(StateName);
        return OFS_AppState<SimulatorDefaultConfigState>(handle).Get();
    }
};

REFL_TYPE(SimulatorDefaultConfigState)
    REFL_FIELD(defaultState)
REFL_END

REFL_TYPE(SimulatorState)
	REFL_FIELD(P1)
	REFL_FIELD(P2)
	REFL_FIELD(Width)
	REFL_FIELD(BorderWidth)
	REFL_FIELD(LineWidth)
	REFL_FIELD(ExtraLineWidth)
	REFL_FIELD(Text)
	REFL_FIELD(Front)
	REFL_FIELD(Back)
	REFL_FIELD(Border)
	REFL_FIELD(ExtraLines)
	REFL_FIELD(Indicator)
	REFL_FIELD(RodColor)
	REFL_FIELD(ShowRod)
	REFL_FIELD(ShowRim)
	REFL_FIELD(ShowTwistIndicator)
	REFL_FIELD(GlobalOpacity)
	REFL_FIELD(EnableIndicators)
	REFL_FIELD(EnablePosition)
	REFL_FIELD(EnableHeightLines)
	REFL_FIELD(ExtraLinesCount)
	REFL_FIELD(LockedPosition)
	REFL_FIELD(FitToPlayer)
	REFL_FIELD(FitAnchor)
	REFL_FIELD(FitSize)
	REFL_FIELD(ShowMultiAxis)
	REFL_FIELD(ModelScale)
	REFL_FIELD(ShowAxisReadout)
	REFL_FIELD(CameraElevation)
	REFL_FIELD(CameraYaw)
	REFL_FIELD(CutawayCase)
	REFL_FIELD(StrokeMin)
	REFL_FIELD(StrokeMax)
REFL_END