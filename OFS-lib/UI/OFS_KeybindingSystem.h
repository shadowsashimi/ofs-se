#pragma once
#include <cstdint>
#include <unordered_map>
#include "state/states/KeybindingState.h"

using ActionFireFn = std::function<void()>;
struct OFS_Action
{
    std::string Id;
    ActionFireFn Action = []() { FUN_ASSERT(false, "Action not set.") };
    bool Dynamic = false;

    OFS_Action(const char* strId, ActionFireFn&& actionFn, bool isDynamic = false) noexcept
        : Id(strId), Action(std::move(actionFn)), Dynamic(isDynamic)
    {
    }

    inline bool operator==(const OFS_Action& b) const noexcept
    {
        return Id == b.Id;
    }
    inline bool operator<(const OFS_Action& b) const noexcept
    {
        return Id < b.Id;
    }
};

struct OFS_ActionGroup
{
    std::string Id;
    TrString GroupName;
    std::vector<uint32_t> actionUiIndices; 
};

struct OFS_ActionUI
{
    std::string ActionId;
    TrString Name;
};

enum class KeyModalType : uint8_t
{
    None,
    Edit,
    New
};

class OFS_KeybindingSystem
{
    private:
    uint32_t stateHandle = 0xFFFF'FFFF;
    std::vector<OFS_ActionGroup> actionGroups;
    std::vector<OFS_ActionUI> actionUI;
    std::unordered_map<std::string, OFS_Action> actions;

    OFS_ActionTrigger tmpTrigger;
    OFS_ActionTrigger editingTrigger;
    std::string editingActionId;
    KeyModalType currentModal = KeyModalType::None;
    // Set by a click in the table, and acted on by the window around it. The
    // table scrolls, so it is a child window with an ID scope of its own, and
    // a popup opened from inside it has a different ID from the one the
    // window then draws: the key capture never appeared, so a binding could
    // not be changed or added at all.
    bool openTriggerModal = false;

    bool showMainModal = false;
    std::string actionFilter;

    vector_set<OFS_ActionTrigger> orphanTriggers;

    // One string per action id, rewritten on every lookup. The menu hands the
    // pointer straight to ImGui alongside a label that is itself formatted
    // into the shared FMT buffer, so the text has to live somewhere else.
    // Unordered map nodes do not move on rehash, which keeps the pointer valid.
    std::unordered_map<std::string, std::string> bindingStrings;

    void addTrigger(const OFS_ActionTrigger& newTrigger) noexcept;
    void editTrigger(const OFS_ActionTrigger& oldTrigger, const OFS_ActionTrigger& editTrigger) noexcept;

    void renderGroup(OFS_KeybindingState& state, OFS_ActionGroup& group) noexcept;
    KeyModalType renderActionRow(OFS_ActionUI& ui) noexcept;
    void renderNewTriggerModal() noexcept;

    public:
    OFS_KeybindingSystem() noexcept;
    ~OFS_KeybindingSystem() noexcept;

    void ProcessKeybindings() noexcept;

    void ShowModal() noexcept;
    void RenderKeybindingWindow() noexcept;

    // The shortcut text for an action, as a menu shows it, or null when the
    // action has no keyboard binding.
    const char* GetBindingString(const char* actionId) noexcept;

    // Runs an action by id as if its binding had been pressed. False when no
    // action has that id.
    bool Invoke(const char* actionId) noexcept;

    void RegisterGroup(const char* groupId, TrString groupName) noexcept;
    void RegisterAction(OFS_Action&& action, TrString name, const char* groupId, const std::vector<OFS_ActionTrigger>& defaultTriggers = std::vector<OFS_ActionTrigger>()) noexcept;
};