#include "OFS_Profiling.h"
#include "OFS_UndoSystem.h"
#include "FunscriptUndoSystem.h"
#include "OFS_Localization.h"

#include <algorithm>
#include <array>

// One name per StateType, in the same order. The older ones are translated;
// newer ones are written here in English, which is where new UI text goes.
struct StateDescription
{
    Tr translated;
    const char* plain;
};

static const char* describeState(uint32_t typeIdx) noexcept;

static std::array<Tr, (int32_t)StateType::CUSTOM_LUA + 1> stateTranslations = {
    Tr::ADD_EDIT_ACTIONS,
    Tr::ADD_EDIT_ACTION,
    Tr::ADD_ACTION,

    Tr::REMOVE_ACTIONS,
    Tr::REMOVE_ACTION,

    Tr::MOUSE_MOVED_ACTIONS,
    Tr::ACTIONS_MOVED,

    Tr::CUT_SELECTION,
    Tr::REMOVE_SELECTION,
    Tr::PASTE_SELECTION,

    Tr::EQUALIZE,
    Tr::INVERT,
    Tr::ISOLATE,

    Tr::TOP_POINTS,
    Tr::MID_POINTS,
    Tr::BOTTOM_POINTS,

    Tr::GENERATE_ACTIONS,
    Tr::FRAME_ALIGN,
    Tr::RANGE_EXTEND,

    Tr::REPEAT_STROKE,
    Tr::MOVE_TO_CURRENT_POSITION,

    Tr::SIMPLIFY,
    Tr::LUA_SCRIPT
};

static std::array<const char*, (int32_t)StateType::TOTAL_UNDOSTATE_TYPES
    - ((int32_t)StateType::CUSTOM_LUA + 1)> plainStateNames = {
    "Beat fill",
    "Depth from loudness",
    "Slow the fast strokes",
    "Thin out close points"
};

static const char* describeState(uint32_t typeIdx) noexcept
{
    if (typeIdx < stateTranslations.size()) return TRD(stateTranslations[typeIdx]);
    const uint32_t plainIdx = typeIdx - (uint32_t)stateTranslations.size();
    FUN_ASSERT(plainIdx < plainStateNames.size(), "out of bounds");
    if (plainIdx >= plainStateNames.size()) return "Edit";
    return plainStateNames[plainIdx];
}

const char* ScriptState::Description() const noexcept
{
    return describeState((uint32_t)type);
}

const char* UndoSystem::UndoContext::Description() const noexcept
{
    return describeState((uint32_t)Type);
}

UndoSystem::UndoSystem() noexcept
{
    RedoStack.reserve(100);
    UndoStack.reserve(1000);
}

void UndoSystem::ShowUndoRedoHistory(bool* open) noexcept
{
    if (!*open) return;
    OFS_PROFILE(__FUNCTION__);
    // Sized by wherever it is docked. It used to pin itself to 200 pixels
    // wide and auto resize, which fought the dock it sat in.
    ImGui::Begin(TR_ID(UndoSystem::WindowId, Tr::UNDO_REDO_HISTORY), open, ImGuiWindowFlags_None);
    dropStale();

    if (UndoStack.empty() && RedoStack.empty()) {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Nothing to undo yet. Each edit is listed here as it is made, "
                            "newest first, with repeats of the same edit counted together.");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }

    // Redo only has entries after an undo, so it only takes space then.
    // Consecutive edits of the same kind are one entry. The next redo is the
    // last element, so reaching the entry starting at i takes size - i redos.
    if (!RedoStack.empty()) ImGui::TextDisabled(TR(REDO_STACK));
    for (int32_t i = 0, size = (int32_t)RedoStack.size(); i < size;) {
        int32_t end = i + 1;
        while (end < size && RedoStack[end].Type == RedoStack[i].Type) end += 1;

        const std::string label = std::string(RedoStack[i].Description()) + " (" + std::to_string(end - i) + ")";
        ImGui::PushID(i);
        // Dimmed, as undone edits are no longer part of the script.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(label.c_str())) PendingRedoSteps = size - i;
        ImGui::PopStyleColor();
        ImGui::PopID();
        i = end;
    }
    if (!RedoStack.empty()) ImGui::Separator();

    // Newest first. Taking back the whole entry covering start..last takes
    // size - start undos.
    ImGui::TextDisabled(TR(UNDO_STACK));
    for (int32_t last = (int32_t)UndoStack.size() - 1; last >= 0;) {
        int32_t start = last;
        while (start > 0 && UndoStack[start - 1].Type == UndoStack[last].Type) start -= 1;

        const std::string label = std::string(UndoStack[last].Description()) + " (" + std::to_string(last - start + 1) + ")";
        ImGui::PushID(1000000 + last);
        if (ImGui::Selectable(label.c_str())) PendingUndoSteps = (int32_t)UndoStack.size() - start;
        ImGui::PopID();
        last = start - 1;
    }

    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Click an entry to undo back to before it, or to redo up to it.");
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void UndoSystem::Snapshot(StateType type, UndoContextScripts&& scriptsToSnapshot, bool clearRedo) noexcept
{
    OFS_PROFILE(__FUNCTION__);
    auto context = UndoStack.emplace_back(std::move(scriptsToSnapshot), type);
    if (clearRedo)
        ClearRedo();

    for (auto& weak : context.Scripts) {
        if (auto script = weak.lock()) {
            script->undoSystem->Snapshot(type, clearRedo);
        }
        else {
            FUN_ASSERT(false, "Stale weak_ptr.");
        }
    }
}

bool UndoSystem::Undo() noexcept
{
    dropStale();
    if (UndoStack.empty()) return false;
    OFS_PROFILE(__FUNCTION__);
    bool undidSomething = false;

    auto context = std::move(UndoStack.back());
    UndoStack.pop_back();
    for (auto& weak : context.Scripts) {
        if (auto script = weak.lock()) {
            undidSomething = script->undoSystem->Undo() || undidSomething;
        }
        else {
            LOG_DEBUG("Stale undo.");
        }
    }

    if (!undidSomething && !UndoStack.empty()) {
        return Undo();
    }

    RedoStack.emplace_back(std::move(context));
    return undidSomething;
}

bool UndoSystem::Redo() noexcept
{
    dropStale();
    if (RedoStack.empty()) return false;
    OFS_PROFILE(__FUNCTION__);
    bool redidSomething = false;

    auto context = std::move(RedoStack.back());
    RedoStack.pop_back();
    for (auto& weak : context.Scripts) {
        if (auto script = weak.lock()) {
            redidSomething = script->undoSystem->Redo() || redidSomething;
        }
        else {
            LOG_DEBUG("Stale redo.");
        }
    }

    if (!redidSomething && !RedoStack.empty()) {
        return Redo();
    }

    UndoStack.emplace_back(std::move(context));
    return redidSomething;
}

void UndoSystem::ClearRedo() noexcept
{
    RedoStack.clear();
}

void UndoSystem::Clear() noexcept
{
    UndoStack.clear();
    RedoStack.clear();
    PendingUndoSteps = 0;
    PendingRedoSteps = 0;
}

void UndoSystem::dropStale() noexcept
{
    auto prune = [](std::vector<UndoContext>& stack) noexcept {
        for (auto& context : stack) {
            auto& scripts = context.Scripts;
            scripts.erase(std::remove_if(scripts.begin(), scripts.end(),
                [](const auto& weak) noexcept { return weak.expired(); }), scripts.end());
        }
        stack.erase(std::remove_if(stack.begin(), stack.end(),
            [](const UndoContext& context) noexcept { return context.Scripts.empty(); }), stack.end());
    };
    prune(UndoStack);
    prune(RedoStack);
}
