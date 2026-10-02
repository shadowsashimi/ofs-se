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
}
