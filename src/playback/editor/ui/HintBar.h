#pragma once

namespace playback::editor::ui {

class HintBar {
public:
    void draw();
    void toggle();
    void setVisible(bool v);
    [[nodiscard]] bool isVisible() const { return mVisible; }

private:
    bool mVisible{true};
};

} // namespace playback::editor::ui