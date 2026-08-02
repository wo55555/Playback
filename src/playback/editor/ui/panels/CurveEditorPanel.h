#pragma once

#include "playback/editor/ui/components/Splitter.h"
#include "playback/editor/ui/components/BezierCurveEditor.h"

namespace playback::editor::ui {

class CurveEditorPanel {
public:
    CurveEditorPanel();

    void draw();
    [[nodiscard]] bool isOpen() const { return mOpen; }
    void setOpen(bool open) { mOpen = open; }

private:
    bool mOpen{false};
    BezierCurveEditor mEditor;
    BezierCurve       mDefaultCurve;
};

} // namespace playback::editor::ui
