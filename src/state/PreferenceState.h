#pragma once
#include <string>
#include "OFS_StateHandle.h"

enum class OFS_Theme : int32_t
{
	// Sashimi takes slot 0 so existing configs that stored "Dark" land on the
	// OFS-SE look by default. Light keeps slot 1, stock Dark moves to the end.
	Sashimi = 0,
	Light = 1,
	Dark = 2
};

struct PreferenceState 
{
	static constexpr auto StateName = "Preferences";

	std::string languageCsv;
	std::string fontOverride;

	int32_t defaultFontSize = 18;
	int32_t currentTheme = static_cast<int32_t>(OFS_Theme::Sashimi);
	// Rounded corners on windows, buttons and the timeline lanes. Off gives
	// the square corners of the original OFS, in any theme.
	bool roundedCorners = true;

	int32_t fastStepAmount = 6;

	int32_t	vsync = 0;
	int32_t framerateLimit = 150;

	bool forceHwDecoding = false;
	// Off by default. The dialog opened itself over every new project to
	// collect fields only the author knows -- creator, tags, performers -- and
	// the one field it could have worked out for itself is now filled in
	// without asking. Preferences turns it back on for anyone who wants the
	// prompt, and the Project menu opens it on demand either way.
	bool showMetaOnNew = false;

	static inline PreferenceState& State(uint32_t stateHandle) noexcept {
		return OFS_AppState<PreferenceState>(stateHandle).Get();
	}
};

REFL_TYPE(PreferenceState)
	REFL_FIELD(languageCsv)
	REFL_FIELD(fontOverride)
	REFL_FIELD(defaultFontSize)
	REFL_FIELD(currentTheme)
	REFL_FIELD(roundedCorners)
	REFL_FIELD(fastStepAmount)
	REFL_FIELD(vsync)
	REFL_FIELD(framerateLimit)
	REFL_FIELD(forceHwDecoding)
	REFL_FIELD(showMetaOnNew)
REFL_END