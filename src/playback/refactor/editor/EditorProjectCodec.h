#pragma once

#include "models/EditorStateExt.h"

#include <optional>
#include <string>
#include <string_view>

namespace playback::refactor::editor {

class EditorProjectCodec {
public:
    static constexpr int kFormatVersion = 3;
    static constexpr std::string_view kEntryName = "editor.bin";

    [[nodiscard]] static std::string encode(const EditorStateExt& state);
    [[nodiscard]] static std::optional<EditorStateExt> decode(std::string_view bytes);
};

} // namespace playback::refactor::editor
