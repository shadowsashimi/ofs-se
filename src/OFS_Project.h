#pragma once
#include "state/ProjectState.h"
#include "Funscript.h"
#include "OFS_Event.h"

#include <vector>
#include <memory>
#include <cstdint>
#include <string>

class ProjectLoadedEvent: public OFS_Event<ProjectLoadedEvent> {
public:
    ProjectLoadedEvent() noexcept {}
};

#define OFS_PROJECT_EXT ".ofsp"

class OFS_Project {
private:
    uint32_t stateHandle = 0xFFFF'FFFF;
    uint32_t bookmarkStateHandle = 0xFFFF'FFFF;

    std::string lastPath;

    std::string notValidError;
    bool valid = false;

    void addError(const std::string& error) noexcept
    {
        valid = false;
        notValidError += "\n";
        notValidError += error;
    }
    void loadNecessaryGlyphs() noexcept;
    void loadMultiAxis(const std::string& rootScript) noexcept;

public:
    static constexpr auto Extension = OFS_PROJECT_EXT;

    // What a project with no media may be set to. The floor keeps the timeline
    // long enough to hold a point; the ceiling is a day, past which the
    // position of a point stops being representable to the millisecond.
    static constexpr float MinStandaloneDuration = 1.f;
    static constexpr float MaxStandaloneDuration = 24.f * 60.f * 60.f;

    OFS_Project() noexcept;
    OFS_Project(const OFS_Project&) = delete;
    OFS_Project(OFS_Project&&) = delete;
    ~OFS_Project() noexcept;

    std::vector<std::shared_ptr<Funscript>> Funscripts;

    bool Load(const std::string& path) noexcept;
    void Save(bool clearUnsavedChanges) noexcept { Save(lastPath, clearUnsavedChanges); }
    void Save(const std::string& path, bool clearUnsavedChanges) noexcept;

    bool ImportFromFunscript(const std::string& path) noexcept;
    bool ImportFromMedia(const std::string& path) noexcept;
    // A project with no media at all: an empty script of a chosen length, for
    // scripting to something played elsewhere, or for laying down a pattern
    // before there is a video to fit it to. A video can be attached later
    // without starting over, see OpenFunscripter::pickDifferentMedia.
    bool CreateStandalone(const std::string& path, float durationSeconds) noexcept;

    bool AddFunscript(const std::string& path) noexcept;
    void RemoveFunscript(int32_t idx) noexcept;

    void Update(float delta, bool idleMode) noexcept;
    void ShowProjectWindow(bool* open) noexcept;
    bool HasUnsavedEdits() noexcept;


    inline void SetActiveIdx(uint32_t activeIdx) noexcept { State().activeScriptIdx = activeIdx; }
    inline uint32_t ActiveIdx() const noexcept { return State().activeScriptIdx; }
    inline std::shared_ptr<Funscript>& ActiveScript() noexcept { return Funscripts[ActiveIdx()]; }

    inline const std::string& Path() const noexcept { return lastPath; }
    // Whether this project has no media behind it. Its length is then whatever
    // was asked for at creation rather than a file's.
    inline bool IsStandalone() const noexcept { return State().relativeMediaPath.empty(); }
    inline bool IsValid() const noexcept { return valid; }
    inline const std::string& NotValidError() const noexcept { return notValidError; }
    inline ProjectState& State() const noexcept { return ProjectState::State(stateHandle); }

    void ExportFunscripts() noexcept;
    void ExportFunscripts(const std::string& outputDir) noexcept;
    // Writes every script into outputDir with the actions given here in place
    // of its own, and changes nothing about the project. For the copy made for
    // a device, which is slower than what is being scripted.
    void ExportFunscriptsLimited(const std::string& outputDir,
        const std::vector<FunscriptArray>& limitedActions) noexcept;
    void ExportFunscript(const std::string& outputPath, int32_t idx) noexcept;
    void ExportFunscript2Quick() noexcept;
    void ExportFunscript11Quick() noexcept;

    std::string MakePathAbsolute(const std::string& relPath) const noexcept;
    std::string MakePathRelative(const std::string& absPath) const noexcept;
    std::string MediaPath() const noexcept;

    template<typename S>
    void serialize(S& s)
    {
        s.ext(*this, bitsery::ext::Growable{},
            [](S& s, OFS_Project& o) {
                s.container(o.Funscripts, 100,
                    [](S& s, std::shared_ptr<Funscript>& script) {
                        s.ext(script, bitsery::ext::StdSmartPtr{});
                    });
            });
    }
};