project(OFS_lib)

set(OFS_LIB_SOURCES
	"event/OFS_EventSystem.cpp"
	"event/OFS_Event.cpp"

	"state/states/KeybindingState.cpp"
	"state/states/ChapterState.cpp"

	"Funscript/Funscript.cpp"
	"Funscript/FunscriptAction.cpp"
	"Funscript/FunscriptUndoSystem.cpp"
	"Funscript/FunscriptHeatmap.cpp"

	"UI/GradientBar.cpp"
	"UI/OFS_SashimiTheme.cpp"
	"UI/OFS_ImGui.cpp"
	"UI/OFS_VideoplayerControls.cpp"
	"UI/OFS_Videopreview.cpp"
	"UI/OFS_BlockingTask.cpp"
	
	"UI/OFS_ScriptTimeline.cpp"
	"UI/ScriptPositionsOverlayMode.cpp"
	"UI/OFS_KeybindingSystem.cpp"
	"UI/OFS_Waveform.cpp"
	
	"videoplayer/OFS_VideoplayerWindow.cpp"
	"videoplayer/impl/OFS_MpvVideoplayer.cpp"

	"state/OFS_StateManager.cpp"
	"state/OFS_LibState.cpp"


	"imgui_impl/imgui_impl_opengl3.cpp"
	"imgui_impl/imgui_impl_sdl.cpp" 

	"gl/OFS_Shader.cpp"

	"OFS_ControllerInput.cpp"

	"OFS_TempoDetection.cpp"
	"OFS_CrashHandler.cpp"
	"OFS_Serialization.cpp"
	"OFS_Util.cpp"
	"OFS_FileLogging.cpp"
	"OFS_DynamicFontAtlas.cpp"
	"OFS_MpvLoader.cpp"

	"OFS_StringsGenerated.cpp"

	"localization/OFS_Localization.cpp"
)

add_library(${PROJECT_NAME} STATIC ${OFS_LIB_SOURCES})
target_include_directories(${PROJECT_NAME} PUBLIC
	"${PROJECT_SOURCE_DIR}/event/"
	"${PROJECT_SOURCE_DIR}/Funscript/"
	"${PROJECT_SOURCE_DIR}/UI/"
	"${PROJECT_SOURCE_DIR}/imgui_impl/"
	"${PROJECT_SOURCE_DIR}/gl/"
	"${PROJECT_SOURCE_DIR}/"
	"${PROJECT_SOURCE_DIR}/player/"
	"${PROJECT_SOURCE_DIR}/localization/"
	"${PROJECT_SOURCE_DIR}/videoplayer/"
	"${PROJECT_SOURCE_DIR}/state/"
	"${CMAKE_CURRENT_BINARY_DIR}"
)

add_custom_command(OUTPUT "OFS_StringsGenerated.cpp" "OFS_StringsGenerated.h"
                   COMMAND gen_localization ARGS "${CMAKE_SOURCE_DIR}/localization/localization.csv"
                   MAIN_DEPENDENCY "${CMAKE_SOURCE_DIR}/localization/localization.csv"
                   DEPENDS gen_localization
                   VERBATIM)

target_link_libraries(${PROJECT_NAME} PUBLIC
	SDL2-static
	SDL2main
	nlohmann_json
	glad2
	imgui
	imgui_stdlib
	tinyfiledialogs
	glm
	bitsery
	refl-cpp
	eventpp
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)

# The version in the title bar and about box comes from a header written on
# every build, so it names the commit actually built. See cmake/GitVersion.cmake.
find_package(Git)
# Shown when there are no git tags to name the version, which is the usual case.
set(OFS_SE_VERSION "1.0" CACHE STRING "Version shown when git has no tag to offer")
set(OFS_GIT_VERSION_DIR "${CMAKE_BINARY_DIR}/generated")
set(OFS_GIT_VERSION_HEADER "${OFS_GIT_VERSION_DIR}/OFS_GitVersion.h")
set(OFS_GIT_VERSION_ARGS
	"-DGIT=${GIT_EXECUTABLE}"
	"-DSRC=${CMAKE_SOURCE_DIR}"
	"-DOUT=${OFS_GIT_VERSION_HEADER}"
	"-DFALLBACK_TAG=${OFS_SE_VERSION}"
	-P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitVersion.cmake")
# Once now, so the header exists before anything first includes it.
execute_process(COMMAND ${CMAKE_COMMAND} ${OFS_GIT_VERSION_ARGS})
add_custom_target(OFS_GitVersion
	COMMAND ${CMAKE_COMMAND} ${OFS_GIT_VERSION_ARGS}
	BYPRODUCTS "${OFS_GIT_VERSION_HEADER}"
	COMMENT "Reading the git commit being built")
add_dependencies(${PROJECT_NAME} OFS_GitVersion)
target_include_directories(${PROJECT_NAME} PUBLIC "${OFS_GIT_VERSION_DIR}")

target_compile_definitions(${PROJECT_NAME} PUBLIC 
	"IMGUI_IMPL_OPENGL_LOADER_GLAD2"
	"MPV_ENABLE_DEPRECATED=0"
)

if(OFS_PROFILE)
	target_compile_definitions(${PROJECT_NAME} PUBLIC OFS_PROFILE_ENABLED=1)
	target_link_libraries(${PROJECT_NAME} PUBLIC tracy)
	message("== ${PROJECT_NAME} - Profiling enabled.")
else()
	target_compile_definitions(${PROJECT_NAME} PUBLIC OFS_PROFILE_ENABLED=0)
endif()


if(WIN32)
	target_include_directories(${PROJECT_NAME} PUBLIC 
		"../lib/libmpv/include"
	)
	target_compile_definitions(${PROJECT_NAME} PUBLIC
		"NOMINMAX"
	)
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
	# linux etc. 
	find_package(PkgConfig REQUIRED) 
	pkg_check_modules(mpv REQUIRED IMPORTED_TARGET mpv)
	
	# mpv is required for the headers
	# but not linked
	# instead libmpv.so.1 is loaded at runtime
	# this avoids linking issues with mpv and lua symbols
elseif(APPLE)
	execute_process(
		COMMAND brew --prefix mpv
		RESULT_VARIABLE BREW_MPV
		OUTPUT_VARIABLE BREW_MPV_PREFIX
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if (BREW_MPV EQUAL 0 AND EXISTS "${BREW_MPV_PREFIX}")
		message(STATUS "Found mpv installed by Homebrew at ${BREW_MPV_PREFIX}")
		set(MPV_INCLUDE "${BREW_MPV_PREFIX}/include")
		set(MPV_LIB "${BREW_MPV_PREFIX}/lib")
	endif()
	target_include_directories(${PROJECT_NAME} PUBLIC
		"${MPV_INCLUDE}"
	)
endif()