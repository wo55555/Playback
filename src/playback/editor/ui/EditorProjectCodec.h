#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"

#include <optional>
#include <string>
#include <string_view>

namespace playback::editor::ui {

class EditorProjectCodec {
public:
    static constexpr int kFormatVersion = 3;
    static constexpr std::string_view kEntryName = "editor.bin";

    [[nodiscard]] static std::string encode(const editing::model::EditorStateExt& state);
    [[nodiscard]] static std::optional<editing::model::EditorStateExt> decode(std::string_view bytes);
};

} // namespace playback::editor::ui
