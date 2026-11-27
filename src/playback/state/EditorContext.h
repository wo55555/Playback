#pragma once

#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"

#include <cstddef>
#include <mutex>
#include <vector>

namespace playback::state {

class EditorContext {
public:
    [[nodiscard]] EditorState snapshot() const;

    void publish(EditorState state);

    void submit(EditorAction action);

    void undo(size_t steps = 1);

    void redo(size_t steps = 1);

    [[nodiscard]] std::vector<EditorAction> takeActions();

    void reset();

private:
    mutable std::mutex        mMutex;
    EditorState               mState;
    std::vector<EditorAction> mPendingActions;
};

} // namespace playback::state
