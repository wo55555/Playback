#pragma once

#include <filesystem>
#include <vector>

namespace playback::exporting {

// Written before Eco Mode is switched off so a crash during the export can be undone on the next launch.
[[nodiscard]] bool writeEcoModeJournal() noexcept;

void clearEcoModeJournal() noexcept;

// Journals of this game install whose owning process is gone.
[[nodiscard]] std::vector<std::filesystem::path> findInterruptedEcoModeJournals() noexcept;

void discardEcoModeJournals(std::vector<std::filesystem::path> const& journals) noexcept;

} // namespace playback::exporting
