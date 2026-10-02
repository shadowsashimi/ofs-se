#include "OFS_KeybindingSystem.h"
#include "OFS_Util.h"
#include "OFS_Localization.h"
#include "OFS_ImGui.h"
#include "imgui_stdlib.h"

#include <array>

static constexpr std::array<ImGuiKey, 12> ModifierKeys
{
    ImGuiKey_LeftCtrl, ImGuiKey_LeftShift, ImGuiKey_LeftAlt, ImGuiKey_LeftSuper,
    ImGuiKey_RightCtrl, ImGuiKey_RightShift, ImGuiKey_RightAlt, ImGuiKey_RightSuper,
    ImGuiKey_ReservedForModCtrl, ImGuiKey_ReservedForModShift, ImGuiKey_ReservedForModAlt, ImGuiKey_ReservedForModSuper
};

static std::array<uint64_t, 4> DissallowedTriggers = 
{
    OFS_ActionTrigger{ ImGuiKey_None, ImGuiKey_MouseLeft }.Hash(),
    OFS_ActionTrigger{ ImGuiKey_None, ImGuiKey_MouseRight }.Hash(),
    OFS_ActionTrigger{ ImGuiKey_None, ImGuiKey_MouseMiddle }.Hash(),
    OFS_ActionTrigger{ ImGuiKey_None, ImGuiKey_Escape }.Hash(),
};

inline static bool isAllowedTrigger(const OFS_ActionTrigger& trigger) noexcept
{
    return std::find(DissallowedTriggers.begin(), DissallowedTriggers.end(), trigger.Hash()) == DissallowedTriggers.end();
}

inline static bool isModifierKey(ImGuiKey key) noexcept
{
    return std::find(ModifierKeys.begin(), ModifierKeys.end(), key) != ModifierKeys.end();
}

static std::string triggerText(const OFS_ActionTrigger& trigger) noexcept
{
    std::string text;
    auto add = [&text](const char* part) noexcept {
        if(!text.empty()) text += '+';
        text += part;
    };

    if(trigger.Mod != ImGuiKey_None)
    {
        if(trigger.Mod & ImGuiMod_Ctrl) add(TR(KEY_MOD_CTRL));
        if(trigger.Mod & ImGuiMod_Alt) add(TR(KEY_MOD_ALT));
        if(trigger.Mod & ImGuiMod_Shift) add(TR(KEY_MOD_SHIFT));
    }

    if(trigger.Key != ImGuiKey_None)
    {
        add(ImGui::GetKeyName(trigger.ImKey()));
    }
    return text;
}

inline static const char* getTriggerText(const OFS_ActionTrigger& trigger) noexcept
{
    FMT("%s", triggerText(trigger).c_str());
    return Util::FormatBuffer;
}

bool OFS_KeybindingSystem::Invoke(const char* actionId) noexcept
{
    auto it = actions.find(actionId);
    if(it == actions.end()) return false;
    it->second.Action();
    return true;
}

const char* OFS_KeybindingSystem::GetBindingString(const char* actionId) noexcept
{
    auto& state = OFS_KeybindingState::State(stateHandle);

    // A gamepad button makes a poor menu hint next to a key, so a keyboard or
    // mouse binding wins when an action has both.
    const OFS_ActionTrigger* found = nullptr;
    for(auto& trigger : state.Triggers)
    {
        if(trigger.MappedActionId != actionId) continue;
        if(!ImGui::IsGamepadKey(trigger.ImKey()))
        {
            found = &trigger;
            break;
        }
        if(found == nullptr) found = &trigger;
    }
    if(found == nullptr) return nullptr;

    auto& text = bindingStrings[actionId];
    text = triggerText(*found);
    return text.c_str();
}

OFS_KeybindingSystem::OFS_KeybindingSystem() noexcept
{
    stateHandle = OFS_AppState<OFS_KeybindingState>::Register(OFS_KeybindingState::StateName);
    auto& state = OFS_KeybindingState::State(stateHandle);
    if(!state.convertedToImGui)
    {
        state.ConvertToImGui();
    }
}

OFS_KeybindingSystem::~OFS_KeybindingSystem() noexcept
{
    auto& state = OFS_KeybindingState::State(stateHandle);
    if(state.convertedToImGui)
    {
        state.ConvertToOFS();
    }
}

void OFS_KeybindingSystem::ProcessKeybindings() noexcept
{
    auto& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) return;

    auto& state = OFS_KeybindingState::State(stateHandle);

    for(int i=0, size = state.Triggers.size(); i < size; i += 1)
    {
        const auto& trigger = state.Triggers[i];
        if((trigger.Mod & ImGuiMod_Mask_) != io.KeyMods)
            continue;

        if(trigger.Key != ImGuiKey_None)
        {
            if(!ImGui::IsKeyPressed(trigger.ImKey(), trigger.ShouldRepeat))
                continue;
        }

        // Only trigger actions when NavMode is disabled
        if(ImGui::IsGamepadKey(trigger.ImKey()) && (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad))
        {
            if(trigger.MappedActionId != "toggle_controller_navmode")
                continue;
        }
        
        // Handle mouse scroll direction
        if(trigger.Key == ImGuiKey_MouseWheelY || trigger.Key == ImGuiKey_MouseWheelX)
        {
            bool direction = trigger.Mod & OFS_ActionTriggerFlags::MouseWheelDirection;
            bool directionMatch = trigger.Key == ImGuiKey_MouseWheelY 
                ? io.MouseWheel > 0.f == direction
                : io.MouseWheelH > 0.f == direction;
            if(!directionMatch) continue;
        }

        // Find action
        auto actionIt = actions.find(trigger.MappedActionId);
        if(actionIt != actions.end())
        {
            // Fire action
            actionIt->second.Action();
        }
        else 
        {
            LOGF_ERROR("Couldn't find action: \"%s\"", trigger.MappedActionId.c_str());
        }
    }
}


inline static OFS_ActionGroup* getGroupById(const char* groupId, std::vector<OFS_ActionGroup>& groups) noexcept
{
    auto it = std::find_if(groups.begin(), groups.end(), 
        [groupId](auto& group) 
        {
            return group.Id == groupId;
        });
    if(it != groups.end()) return &(*it);
    return nullptr;
}

void OFS_KeybindingSystem::RegisterGroup(const char* groupId, TrString groupName) noexcept
{
    FUN_ASSERT(getGroupById(groupId, actionGroups) == nullptr, "Group already registered");
    actionGroups.emplace_back(OFS_ActionGroup{groupId, groupName});
}

void OFS_KeybindingSystem::RegisterAction(OFS_Action&& action, TrString name, const char* groupId, const std::vector<OFS_ActionTrigger>& defaultTriggers) noexcept
{
    auto& state = OFS_KeybindingState::State(stateHandle);
    auto group = getGroupById(groupId, actionGroups);
    FUN_ASSERT(group, "Couldn't find group.");

    auto it = actions.emplace(std::move(std::make_pair(action.Id, std::move(action))));

    if(it.second)
    {
        uint32_t uiIdx = actionUI.size();
        auto& ui = actionUI.emplace_back();
        ui.ActionId = it.first->second.Id;
        ui.Name = std::move(name);
        group->actionUiIndices.emplace_back(uiIdx);

        for(auto& trigger : defaultTriggers)
        {
            auto it = state.Triggers.find(trigger);
            if(it == state.Triggers.end())
            {
                auto newTrigger = trigger;
                newTrigger.MappedActionId = ui.ActionId;
                state.Triggers.emplace(newTrigger);
            }
        }
    }
    else 
    {
        FUN_ASSERT(it.first->second.Dynamic, "This shouldn't happen for static actions.");
        LOGF_DEBUG("Action \"%s\" already exists", it.first->second.Id.c_str());
    }
}

void OFS_KeybindingSystem::addTrigger(const OFS_ActionTrigger& newTrigger) noexcept
{
    auto& state = OFS_KeybindingState::State(stateHandle);
    auto it = state.Triggers.find(newTrigger);    
    if(it == state.Triggers.end())
    {
        state.Triggers.emplace(newTrigger);
    }
    else 
    {
        // if they are the same do nothing
        if(it->Hash() == newTrigger.Hash() && it->MappedActionId == newTrigger.MappedActionId)
            return;

        std::stringstream ss;
        ss << '[' << getTriggerText(newTrigger) << ']';
        ss << '\n' << "Is already in use for {" << it->MappedActionId << '}';
        ss << '\n' << "Do you want to remove the existing trigger?";
        

        Util::YesNoCancelDialog("Trigger is already in use",
            ss.str(),
            [stateHandle = stateHandle, newTrigger](auto result)
            {
                if(result == Util::YesNoCancel::Yes)
                {
                    auto& state = OFS_KeybindingState::State(stateHandle);
                    auto it = state.Triggers.find(newTrigger);
                    if(it != state.Triggers.end())
                    {
                        it->MappedActionId = newTrigger.MappedActionId;
                    }
                }
            });
    }
}

void OFS_KeybindingSystem::editTrigger(const OFS_ActionTrigger& oldTrigger, const OFS_ActionTrigger& newTrigger) noexcept
{
    // The bindings are a set sorted by key, so a binding is moved by taking it
    // out and putting it back in. Changed where it stood, it was left out of
    // order, where looking it up by its key could miss it; and nothing checked
    // whether the new key was already doing something else.
    auto& state = OFS_KeybindingState::State(stateHandle);
    auto oldIt = state.Triggers.find(oldTrigger);
    if(oldIt == state.Triggers.end())
    {
        addTrigger(newTrigger);
        return;
    }
    if(oldIt->Hash() == newTrigger.Hash()) return;

    const bool repeat = oldIt->ShouldRepeat;
    auto taken = state.Triggers.find(newTrigger);
    if(taken == state.Triggers.end())
    {
        state.Triggers.erase(oldIt);
        auto moved = newTrigger;
        moved.ShouldRepeat = repeat;
        state.Triggers.emplace(moved);
        return;
    }
    if(taken->MappedActionId == newTrigger.MappedActionId)
    {
        // The action already has that key: this one is simply no longer needed.
        state.Triggers.erase(oldIt);
        return;
    }

    std::stringstream ss;
    ss << '[' << getTriggerText(newTrigger) << ']';
    ss << '\n' << "Is already in use for {" << taken->MappedActionId << '}';
    ss << '\n' << "Do you want to use it for {" << newTrigger.MappedActionId << "} instead?";
    Util::YesNoCancelDialog("Trigger is already in use",
        ss.str(),
        [stateHandle = stateHandle, oldTrigger, newTrigger](auto result)
        {
            if(result != Util::YesNoCancel::Yes) return;
            auto& state = OFS_KeybindingState::State(stateHandle);
            auto oldIt = state.Triggers.find(oldTrigger);
            if(oldIt != state.Triggers.end()) state.Triggers.erase(oldIt);
            auto taken = state.Triggers.find(newTrigger);
            if(taken != state.Triggers.end()) taken->MappedActionId = newTrigger.MappedActionId;
        });
}

void OFS_KeybindingSystem::renderNewTriggerModal() noexcept
{
    if(ImGui::BeginPopupModal(TR_ID("ADD_EDIT_TRIGGER", Tr::ADD_EDIT_TRIGGER), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextDisabled("[%s]", editingActionId.c_str());
        ImGui::TextUnformatted(TR(CHANGE_KEY_MSG));

        ImGui::TextUnformatted(getTriggerText(tmpTrigger));

        const auto oldMod = tmpTrigger.Mod;
        const auto oldKey = tmpTrigger.Key;
        tmpTrigger.Mod = ImGuiKey_None;
        tmpTrigger.Key = ImGuiKey_None;

        auto& io = ImGui::GetIO();
        bool timerShouldReset = true;
        for(int idx = 0; idx < ImGuiKey_KeysData_SIZE; idx += 1)
        {
            auto key = (ImGuiKey)(ImGuiKey_NamedKey_BEGIN + idx);
            if(ImGui::IsKeyDown(key))
            {
                if(key == ImGuiKey_Escape) 
                {
                    ImGui::CloseCurrentPopup();
                    break;
                }

                if(!isModifierKey(key))
                {
                    bool unchanged = oldKey == key && oldMod == (ImGuiKey)io.KeyMods;
                    tmpTrigger.Key = key;
                    tmpTrigger.Mod = (ImGuiKey)io.KeyMods;
                    
                    if(key == ImGuiKey_MouseWheelY || key == ImGuiKey_MouseWheelX)
                    {
                        bool direction = key == ImGuiKey_MouseWheelY
                            ? io.MouseWheel > 0.f
                            : io.MouseWheelH > 0.f;
                        tmpTrigger.SetFlag(OFS_ActionTriggerFlags::MouseWheelDirection, direction);
                    }

                    if(isAllowedTrigger(tmpTrigger)) 
                    {
                        tmpTrigger.MappedActionId = editingActionId;
                        switch (currentModal)
                        {
                            case KeyModalType::New:
                                addTrigger(tmpTrigger);
                                break;
                            case KeyModalType::Edit:
                                editTrigger(editingTrigger, tmpTrigger);
                                break;
                            default: 
                                FUN_ASSERT(false, "unreachable");
                        }
                        ImGui::CloseCurrentPopup();
                    }

                    break;
                }
                else 
                {
                    tmpTrigger.Mod = (ImGuiKey)io.KeyMods;
                    timerShouldReset = true;
                    // No break here
                }
            }
        }
        
        ImGui::EndPopup();
    }
}

// A binding is a small button in the action's row: click it to rebind, right
// click it for repeat and delete. Every binding an action has is visible at
// once, where it used to take opening a tree node per action to find out
// whether it had any at all.
KeyModalType OFS_KeybindingSystem::renderActionRow(OFS_ActionUI& ui) noexcept
{
    auto& state = OFS_KeybindingState::State(stateHandle);
    auto keyModal = KeyModalType::None;
    const auto& style = ImGui::GetStyle();

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ui.Name.c_str());

    ImGui::TableNextColumn();
    ImGui::PushID(ui.ActionId.c_str());

    // Buttons wrap inside the cell rather than running off its edge.
    bool first = true;
    auto placeNext = [&first, &style](const char* label) noexcept {
        if(!first)
        {
            ImGui::SameLine();
            const float width = ImGui::CalcTextSize(label, nullptr, true).x + (style.FramePadding.x * 2.f);
            if(ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
        }
        first = false;
    };

    int32_t deleteIdx = -1;
    for(int32_t i = 0, size = (int32_t)state.Triggers.size(); i < size; i += 1)
    {
        auto& trigger = state.Triggers[i];
        if(trigger.MappedActionId != ui.ActionId) continue;

        ImGui::PushID(i);
        const auto text = triggerText(trigger);
        const char* label = trigger.ShouldRepeat
            ? FMT("%s " ICON_REFRESH "###binding", text.c_str())
            : FMT("%s###binding", text.c_str());
        placeNext(label);
        if(ImGui::Button(label))
        {
            editingTrigger = trigger;
            keyModal = KeyModalType::Edit;
        }
        OFS::Tooltip(trigger.ShouldRepeat
            ? "Click to rebind, right click for options. Repeats while held."
            : "Click to rebind, right click for options.");

        if(ImGui::BeginPopupContextItem("##bindingMenu"))
        {
            ImGui::Checkbox(TR(REPEAT), &trigger.ShouldRepeat);
            if(ImGui::MenuItem(FMT("%s " ICON_TRASH, TR(DELETE))))
            {
                deleteIdx = i;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    const bool unbound = first;
    const char* addLabel = unbound ? "+ Add###add" : "+###add";
    placeNext(addLabel);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if(ImGui::Button(addLabel))
    {
        keyModal = KeyModalType::New;
    }
    ImGui::PopStyleColor();
    OFS::Tooltip(unbound ? "This action has no binding yet." : "Add another binding.");

    ImGui::PopID();

    if(deleteIdx >= 0)
    {
        state.Triggers.erase(state.Triggers.begin() + deleteIdx);
    }
    return keyModal;
}

void OFS_KeybindingSystem::renderGroup(OFS_KeybindingState& state, OFS_ActionGroup& group) noexcept
{
    // The filter matches the action's name, its group's name, or the text of
    // any of its bindings, so typing "Ctrl" lists everything on Ctrl.
    auto matches = [&](const OFS_ActionUI& ui) noexcept {
        if(actionFilter.empty()) return true;
        if(Util::ContainsInsensitive(ui.Name.c_str(), actionFilter.c_str())) return true;
        if(Util::ContainsInsensitive(group.GroupName.c_str(), actionFilter.c_str())) return true;
        for(auto& trigger : state.Triggers)
        {
            if(trigger.MappedActionId == ui.ActionId
                && Util::ContainsInsensitive(triggerText(trigger).c_str(), actionFilter.c_str()))
                return true;
        }
        return false;
    };

    // Filter before drawing the header, so a group with nothing left in it
    // leaves no empty heading behind.
    bool anyMatch = false;
    for(uint32_t idx : group.actionUiIndices)
    {
        if(matches(actionUI[idx])) { anyMatch = true; break; }
    }
    if(!anyMatch) return;

    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn();
    if(!actionFilter.empty())
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const bool open = ImGui::TreeNodeEx(group.Id.c_str(),
        ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_NoTreePushOnOpen,
        "%s", group.GroupName.c_str());
    ImGui::TableNextColumn();

    if(open)
    {
        for(uint32_t idx : group.actionUiIndices)
        {
            auto& ui = actionUI[idx];
            if(!matches(ui)) continue;
            auto modal = renderActionRow(ui);
            if(modal != KeyModalType::None)
            {
                openTriggerModal = true;
                editingActionId = ui.ActionId;
                tmpTrigger = OFS_ActionTrigger();
                currentModal = modal;
            }
        }
    }
}

void OFS_KeybindingSystem::ShowModal() noexcept
{
    showMainModal = true;
}

static void findOrphanTriggers(const OFS_KeybindingState& state, const std::unordered_map<std::string, OFS_Action>& actions, vector_set<OFS_ActionTrigger>& out) noexcept
{
    out.clear();
    for(auto& trigger : state.Triggers)
    {
        auto it = actions.find(trigger.MappedActionId);
        if(it == actions.end())
        {
            out.emplace(trigger);
        }
    }
}

void OFS_KeybindingSystem::RenderKeybindingWindow() noexcept
{
    if(showMainModal)
    {
        ImGui::OpenPopup(TR_ID("KEYS", Tr::KEYS));
        showMainModal = false;
    }

    bool showWindow = true;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.45f, viewport->WorkSize.y * 0.7f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(viewport->GetWorkCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if(ImGui::BeginPopupModal(TR_ID("KEYS", Tr::KEYS), &showWindow, ImGuiWindowFlags_None))
    {
        auto& state = OFS_KeybindingState::State(stateHandle);

        ImGui::SetNextItemWidth(-1.f);
        if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##filter", "Filter by action, group or key, e.g. Ctrl", &actionFilter);
        ImGui::Spacing();

        // The footer holds the validate button and, when it has found some,
        // the orphaned bindings, so the table leaves room for one row of it.
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
            | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if(ImGui::BeginTable("##bindings", 2, flags, ImVec2(0.f, -footer)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.f);
            ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableHeadersRow();

            for(auto& group : actionGroups)
            {
                renderGroup(state, group);
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        if(ImGui::Button(TR(VALIDATE)))
        {
            findOrphanTriggers(state, actions, orphanTriggers);
            if(orphanTriggers.empty())
            {
                Util::MessageBoxAlert(TR(OK_RESULT), TR(OK_RESULT));
            }
        }

        if(!orphanTriggers.empty())
        {
            ImGui::TextUnformatted(TR(ORPHAN_TRIGGER_MESSAGE));
            int deleteIdx = -1;
            for(int i=0, size=orphanTriggers.size(); i < size; i += 1)
            {
                auto& orphanTrigger = orphanTriggers[i];
                ImGui::Text("%s [%s]", orphanTrigger.MappedActionId.c_str(), getTriggerText(orphanTrigger));
                ImGui::SameLine();
                if(ImGui::Button(FMT("%s " ICON_TRASH, TR(DELETE))))
                {
                    deleteIdx = i;
                }
            }
            if(deleteIdx >= 0) 
            {
                auto it = state.Triggers.find(orphanTriggers[deleteIdx]);
                if(it != state.Triggers.end())
                {
                    state.Triggers.erase(it);
                    findOrphanTriggers(state, actions, orphanTriggers);
                }
            }
        }

        if(openTriggerModal)
        {
            ImGui::OpenPopup(TR_ID("ADD_EDIT_TRIGGER", Tr::ADD_EDIT_TRIGGER));
            openTriggerModal = false;
        }
        renderNewTriggerModal();
        if(!showWindow) 
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}