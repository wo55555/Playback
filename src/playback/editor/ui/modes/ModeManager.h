#pragma once

#include <cstdint>

namespace playback::editor::ui {

enum class EditorMode { Edit, Render };

class ModeManager {
public:
    static ModeManager& getInstance();

    [[nodiscard]] EditorMode current() const { return mCurrent; }
    void                     switchTo(EditorMode mode);

    [[nodiscard]] bool  isTransitioning() const { return mTransitioning; }
    [[nodiscard]] float transitionAlpha() const { return mTransitionAlpha; }

private:
    ModeManager() = default;

    EditorMode mCurrent{EditorMode::Edit};
    bool       mTransitioning{false};
    float      mTransitionAlpha{1.0f};
};

} // namespace playback::editor::ui
