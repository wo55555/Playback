#pragma once

#include "playback/editor/context/EditorAction.h"
#include "playback/editor/context/EditorState.h"

#include <mutex>
#include <vector>

namespace playback::editor {

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

} // namespace playback::editor
