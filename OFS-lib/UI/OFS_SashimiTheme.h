#pragma once

// ============================================================================
//  OFS-SE  --  OpenFunscripter Sashimi Edition
//  Monochrome grey theme with pink as the single UI highlight colour.
//
//  Everything colour related for the app chrome lives here so the whole look
//  can be retuned from one file. Data encoding colours (the heatmap gradient,
//  the tempo subdivision colours) deliberately stay untouched, they carry
//  meaning rather than style.
// ============================================================================

#include "imgui.h"

#include <cstdint>

namespace OFS_Sashimi
{
// ---------------------------------------------------------------- greyscale
// Neutral, no colour cast. Darkest to lightest.
constexpr uint32_t Grey00 = IM_COL32(0x0B, 0x0B, 0x0B, 0xFF); // empty dockspace
constexpr uint32_t Grey05 = IM_COL32(0x12, 0x12, 0x12, 0xFF); // window background
constexpr uint32_t Grey10 = IM_COL32(0x18, 0x18, 0x18, 0xFF); // child background
constexpr uint32_t Grey15 = IM_COL32(0x1F, 0x1F, 0x1F, 0xFF); // frames, popups
constexpr uint32_t Grey20 = IM_COL32(0x26, 0x26, 0x26, 0xFF); // buttons, inactive tabs
constexpr uint32_t Grey25 = IM_COL32(0x2E, 0x2E, 0x2E, 0xFF); // hovered surfaces
constexpr uint32_t Grey30 = IM_COL32(0x38, 0x38, 0x38, 0xFF); // active surfaces, borders
constexpr uint32_t Grey40 = IM_COL32(0x4A, 0x4A, 0x4A, 0xFF); // scrollbar grab
constexpr uint32_t Grey50 = IM_COL32(0x5E, 0x5E, 0x5E, 0xFF); // separators, faint lines
constexpr uint32_t Grey60 = IM_COL32(0x77, 0x77, 0x77, 0xFF); // disabled text
constexpr uint32_t Grey80 = IM_COL32(0xA8, 0xA8, 0xA8, 0xFF); // secondary text
constexpr uint32_t Grey95 = IM_COL32(0xE6, 0xE6, 0xE6, 0xFF); // primary text

// -------------------------------------------------------------------- pink
constexpr uint32_t PinkDeep   = IM_COL32(0x8C, 0x2B, 0x51, 0xFF); // pressed / dim fills
constexpr uint32_t Pink       = IM_COL32(0xE8, 0x54, 0x8A, 0xFF); // THE highlight colour
constexpr uint32_t PinkBright = IM_COL32(0xFF, 0x70, 0xA2, 0xFF); // hover
constexpr uint32_t PinkPale   = IM_COL32(0xFF, 0x9C, 0xC0, 0xFF); // active / on-dark text

// Translucent variants used for fills behind text.
constexpr uint32_t PinkFill   = IM_COL32(0xE8, 0x54, 0x8A, 0x59); // ~35%
constexpr uint32_t PinkFillHi = IM_COL32(0xE8, 0x54, 0x8A, 0x8C); // ~55%

// Convert a packed IM_COL32 to the ImVec4 form ImGuiStyle wants.
inline ImVec4 V4(uint32_t col) noexcept
{
    return ImVec4(
        ((col >> IM_COL32_R_SHIFT) & 0xFF) / 255.f,
        ((col >> IM_COL32_G_SHIFT) & 0xFF) / 255.f,
        ((col >> IM_COL32_B_SHIFT) & 0xFF) / 255.f,
        ((col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f);
}

// Same colour, different alpha.
inline ImVec4 V4(uint32_t col, float alpha) noexcept
{
    auto v = V4(col);
    v.w = alpha;
    return v;
}

// Applies the full monochrome-grey/pink palette to an ImGui style.
void ApplyStyle(ImGuiStyle& style) noexcept;

// Rounded corners, or the square ones of the original OFS. Applies to any
// theme, so it is called after the theme's colours are set.
void SetRounding(ImGuiStyle& style, bool rounded) noexcept;

// The colours OFS-SE's own widgets draw with, by what they are for rather than
// by shade. Under the Sashimi theme they are the palette above; under Dark and
// Light they are worked out from the ImGui style, so a segmented bar or a lit
// toggle follows the theme instead of staying grey and pink on a white UI.
struct Roles
{
    // The highlight colour and its lighter and darker steps. These also mark
    // things on the timeline, which stays dark in every theme, so Pale and
    // Bright must read on a dark background.
    uint32_t Accent       = Pink;
    uint32_t AccentBright = PinkBright;
    uint32_t AccentDeep   = PinkDeep;
    uint32_t AccentPale   = PinkPale;

    // A selected segment or a toggle that is on.
    uint32_t OnFill   = PinkFill;
    uint32_t OnFillHi = PinkFillHi;
    uint32_t OnText   = PinkPale;
    uint32_t OnBorder = Pink;

    // A segment that is not selected or a toggle that is off.
    uint32_t OffFill       = Grey15;
    uint32_t OffFillHi     = Grey25;
    uint32_t OffFillActive = Grey30;
    uint32_t OffText       = Grey80;
    uint32_t OffBorder     = Grey30;

    // The line between segments and the outline around a segmented bar.
    uint32_t Divider = Grey40;
    uint32_t Outline = Grey50;

    // Dimmed text, such as a paused recording.
    uint32_t TextFaint = Grey60;

    // The active timeline lane and its name pill, tinted toward the accent.
    uint32_t LaneActiveBg   = IM_COL32(0x24, 0x14, 0x1B, 0xFF);
    uint32_t LaneActivePill = IM_COL32(0x2A, 0x14, 0x1E, 0xC8);
};

// The roles for the theme in use.
const Roles& Role() noexcept;

// Recomputes the roles. Call after a theme has been applied to the style.
void UpdateRoles(const ImGuiStyle& style, bool sashimi) noexcept;
}
