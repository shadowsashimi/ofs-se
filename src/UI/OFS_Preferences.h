#pragma once

#include "nlohmann/json.hpp"

#include <sstream>
#include <vector>
#include <string>

#include "OFS_ScriptSimulator.h"

#include "OFS_Reflection.h"
#include "OFS_Util.h"
#include "imgui.h"

#include "OFS_ScriptPositionsOverlays.h"

#include "state/PreferenceState.h"

class OFS_Preferences
{
private:
	uint32_t prefStateHandle = 0xFFFF'FFFF;
	std::vector<std::string> translationFiles;
	// Which sidebar section is showing. Not saved: the window opens where it
	// was left for the session, and on Application after a restart.
	int32_t activeSection = 0;
public:
	inline uint32_t StateHandle() const noexcept { return prefStateHandle; }
public:
	bool ShowWindow = false;
	OFS_Preferences() noexcept;
	bool ShowPreferenceWindow() noexcept;
	// Opens the window on the timeline's section.
	void OpenTimelineSection() noexcept;
	void SetTheme(OFS_Theme theme) noexcept;
};
