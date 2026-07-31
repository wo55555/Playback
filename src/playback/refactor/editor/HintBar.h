#pragma once

namespace playback::refactor::editor {

class HintBar {
public:
    void draw();
    void toggle();
    void setVisible(bool v);
    [[nodiscard]] bool isVisible() const { return mVisible; }

private:
    bool mVisible{true};
};

} // namespace playback::refactor::editor