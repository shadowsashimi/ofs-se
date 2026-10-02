#pragma once
#include <vector>
#include <memory>

#include "FunscriptUndoSystem.h"

enum StateType : int32_t {
    ADD_EDIT_ACTIONS = 0,
    ADD_EDIT_ACTION = 1,
    ADD_ACTION = 2,

    REMOVE_ACTIONS = 3,
    REMOVE_ACTION = 4,

    MOUSE_MOVE_ACTION = 5,
    ACTIONS_MOVED = 6,

    CUT_SELECTION = 7,
    REMOVE_SELECTION = 8,
    PASTE_COPIED_ACTIONS = 9,

    EQUALIZE_ACTIONS = 10,
    INVERT_ACTIONS = 11,
    ISOLATE_ACTION = 12,

    TOP_POINTS_ONLY = 13,
    MID_POINTS_ONLY = 14,
    BOTTOM_POINTS_ONLY = 15,

    GENERATE_ACTIONS = 16,
    FRAME_ALIGN = 17, // unused
    RANGE_EXTEND = 18,

    REPEAT_STROKE = 19,

    MOVE_ACTION_TO_CURRENT_POS = 20,

    SIMPLIFY = 21,
    CUSTOM_LUA = 22,

    BEAT_FILL = 23,
    LOUDNESS_DEPTH = 24,
    LIMIT_SPEED = 25,
    THIN_OUT = 26,
    // add more here & name them in stateDescriptions in OFS_UndoSystem.cpp


    TOTAL_UNDOSTATE_TYPES
};

using UndoContextScripts = std::vector<std::weak_ptr<const class Funscript>>;

// this manages undo/redo accross the whole app
class UndoSystem {
private:
    struct UndoContext {
        int32_t Type;
        UndoContextScripts Scripts;
        UndoContext(UndoContextScripts&& scripts, StateType type) noexcept
        : Scripts(std::move(scripts)), Type((int32_t)type)
        {}

        const char* Description() const noexcept;
    };

    std::vector<UndoContext> UndoStack;
    std::vector<UndoContext> RedoStack;
    void ClearRedo() noexcept;
    // Forgets entries whose scripts have all been closed or removed. They can
    // no longer be undone, yet were still listed, and undo stepped over them
    // silently, which took an extra step back for no visible reason.
    void dropStale() noexcept;

public:
    UndoSystem() noexcept;
    static constexpr const char* WindowId = "###UNDO_REDO_HISTORY";
    void ShowUndoRedoHistory(bool* open) noexcept;

    // Set when an entry in the history window is clicked: how many undos or
    // redos it takes to get back to it. The app carries them out after the
    // window has drawn, since an undo also touches things beyond the scripts
    // this class knows about, and changing the stacks mid-draw would pull the
    // list out from under the loop drawing it.
    int32_t PendingUndoSteps = 0;
    int32_t PendingRedoSteps = 0;

    void Snapshot(StateType type, std::weak_ptr<const class Funscript> scriptToSnapshot, bool clearRedo = true) noexcept
    {
        Snapshot(type, std::move(UndoContextScripts{ scriptToSnapshot }), clearRedo);
    }
    void Snapshot(StateType type,
        UndoContextScripts&& scriptsToSnapshot,
        bool clearRedo = true) noexcept;
    bool Undo() noexcept;
    bool Redo() noexcept;
    // Everything goes: for a project being opened or closed, whose scripts the
    // old entries cannot reach.
    void Clear() noexcept;

    inline bool MatchUndoTop(int32_t type) const noexcept { return !UndoEmpty() && UndoStack.back().Type == type; }
    inline bool MatchRedoTop(int32_t type) const noexcept { return !RedoEmpty() && RedoStack.back().Type == type; }
    inline bool UndoEmpty() const noexcept { return UndoStack.empty(); }
    inline bool RedoEmpty() const noexcept { return RedoStack.empty(); }
};
