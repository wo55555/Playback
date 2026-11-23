#pragma once

#include "ll/api/i18n/I18n.h"

#include <string>
#include <string_view>

namespace playback::editor::ui {

// Resolves a history label key through LeviLamina's active locale, returning the key as-is if missing.
[[nodiscard]] inline std::string translateHistoryLabel(std::string_view key) {
    if (key.empty()) return {};
    std::string_view const translated = ll::i18n::getInstance().get(key, {});
    return translated.empty() ? std::string(key) : std::string(translated);
}

} // namespace playback::editor::ui
