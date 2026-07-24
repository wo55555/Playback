#pragma once

#include "playback/editor/context/EditorContext.h"

namespace playback::editor {

class EditorController {
public:
    explicit EditorController(EditorContext& context);

    void tick(bool hudVisible);

private:
    EditorContext& mContext;
};

} // namespace playback::editor
