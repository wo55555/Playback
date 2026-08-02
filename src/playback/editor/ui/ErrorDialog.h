#pragma once

#include <string>

namespace playback::editor::ui {

class ErrorDialog {
public:
    static ErrorDialog& getInstance();

    void show(std::string_view title, std::string_view msg);
    void draw();  // Must be called within EditMode / RenderMode draw

private:
    ErrorDialog() = default;

    bool        mOpen{false};
    std::string mTitle;
    std::string mMsg;
};

} // namespace playback::editor::ui