#pragma once

#include <cstdint>

// The TCode machines OFS-SE knows by name, and the axes each one has, by the
// name OFS gives an axis script. Picking one in the Tracks panel adds its
// tracks; picking one in the Devices panel turns on its axes and no others.
//
// Only which axes a machine has is set here. Travel and speed limits depend on
// how a machine is built and tuned, so those stay the user's to set.
struct TCodeMachine
{
    const char* name;
    const char* tip;
    const char* axes[7];
    int32_t axisCount;
};

inline constexpr TCodeMachine TCodeMachines[] = {
    { "SSR1", "A single stroke axis.",
        { "stroke" }, 1 },
    { "OSR2", "Stroke, roll and pitch.",
        { "stroke", "roll", "pitch" }, 3 },
    { "OSR2 with twist", "Stroke, roll and pitch, and a twist head.",
        { "stroke", "twist", "roll", "pitch" }, 4 },
    { "SR6", "Stroke, surge, sway, roll and pitch.",
        { "stroke", "surge", "sway", "roll", "pitch" }, 5 },
    { "SR6 with twist", "Stroke, surge, sway, roll and pitch, and a twist head.",
        { "stroke", "surge", "sway", "twist", "roll", "pitch" }, 6 },
};

inline constexpr int32_t TCodeMachineCount = (int32_t)(sizeof(TCodeMachines) / sizeof(TCodeMachines[0]));
