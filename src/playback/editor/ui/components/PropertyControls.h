#pragma once

#include <cstddef>
#include <string_view>

namespace playback::editor::ui::property {

void beginInspector(std::string_view title, std::string_view objectName);
void searchBar(char const* id, char const* hint, char* buffer, size_t bufferSize);
bool beginSection(char const* label, bool defaultOpen = true);
void endSection();
void textRow(char const* label, char const* value);
void separator();
bool actionButton(char const* label, bool enabled = true);

} // namespace playback::editor::ui::property
