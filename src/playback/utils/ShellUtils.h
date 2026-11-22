#pragma once

#include <filesystem>
#include <optional>

namespace playback::utils {

// Opens Explorer on `path`: selects it when it is a file, opens it when it is a folder.
bool revealInExplorer(std::filesystem::path const& path);

// Modal folder picker. Returns nothing when the user cancels or the dialog cannot be shown.
[[nodiscard]] std::optional<std::filesystem::path> pickFolder(std::filesystem::path const& initial);

// Modal audio file picker. Returns nothing when the user cancels or the dialog cannot be shown.
[[nodiscard]] std::optional<std::filesystem::path> pickAudioFile(std::filesystem::path const& initial = {});

} // namespace playback::utils
