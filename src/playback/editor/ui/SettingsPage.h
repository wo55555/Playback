#pragma once

namespace playback::editor::ui {

enum class SettingsSection { Recording, Browser, Editor, Export, General };

void openSettingsPage(SettingsSection section = SettingsSection::Recording);
void closeSettingsPage();
[[nodiscard]] bool isSettingsPageOpen();
void drawSettingsPage();

} // namespace playback::editor::ui
