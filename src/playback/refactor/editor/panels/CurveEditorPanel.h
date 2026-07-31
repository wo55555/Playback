#pragma once

#include "playback/refactor/editor/Splitter.h"
#include "playback/refactor/video-editing/BezierCurveEditor.h"

namespace playback::refactor::editor {

class CurveEditorPanel {
public:
    CurveEditorPanel();

    void draw();
    [[nodiscard]] bool isOpen() const { return mOpen; }
    void setOpen(bool open) { mOpen = open; }

private:
    bool mOpen{false};
    video_editing::BezierCurveEditor mEditor;
    video_editing::BezierCurve mDefaultCurve;
};

} // namespace playback::refactor::editor