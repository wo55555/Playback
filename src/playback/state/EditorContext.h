#pragma once

#include "playback/state/EditorAction.h"
#include "playback/state/EditorState.h"

#include <mutex>
#include <vector>

namespace playback::state {

class EditorContext {
public:
    [[nodiscard]] EditorState snapshot() const;

    void publish(EditorState state);

    void submit(EditorAction action);

    [[nodiscard]] std::vector<EditorAction> takeActions();

    void reset();

private:
    mutable std::mutex        mMutex;
    EditorState               mState;
    std::vector<EditorAction> mPendingActions;
};

} // namespace playback::state
