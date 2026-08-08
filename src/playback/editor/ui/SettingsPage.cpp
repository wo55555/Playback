#include "SettingsPage.h"

#include "playback/Config.h"
#include "playback/Playback.h"
#include "playback/editor/ui/EditorTheme.h"
#include "playback/functions/record/Recorder.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <Windows.h>

#include <algorithm>
#include <string>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

SettingsFonts gFonts;

struct SettingsPageState {
    bool                           open{};
    bool                           initialized{};
    bool                           captureRecordingKey{};
    bool                           capturePauseKey{};
    bool                           closeConfirmation{};
    SettingsSection                section{SettingsSection::Recording};
    config::RecordingControlsConfig draft;
    std::string                    error;
};

SettingsPageState& state() {
    static SettingsPageState value;
    return value;
}

void initializeDraft() {
    auto& page = state();
    if (page.initialized) return;
    page.draft = Playback::getInstance().getConfig().recordingControls;
    page.error.clear();
    page.initialized = true;
}

bool isDirty() {
    auto const& draft = state().draft;
    auto const& saved = Playback::getInstance().getConfig().recordingControls;
    return draft.toggleRecordingKey != saved.toggleRecordingKey || draft.togglePauseKey != saved.togglePauseKey
        || draft.showStatusOverlay != saved.showStatusOverlay || draft.overlayPosition != saved.overlayPosition;
}

void requestClose() {
    auto& page = state();
    if (isDirty()) {
        page.closeConfirmation = true;
        return;
    }
    closeSettingsPage();
}

bool captureKey(UINT& key, bool& capturing) {
    if (!capturing) return false;
    for (int current = 'A'; current <= 'Z'; ++current) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + current - 'A'), false)) {
            key = static_cast<UINT>(current);
            capturing = false;
            return true;
        }
    }
    for (int current = '0'; current <= '9'; ++current) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_0 + current - '0'), false)) {
            key = static_cast<UINT>(current);
            capturing = false;
            return true;
        }
    }
    for (int current = 0; current < 24; ++current) {
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + current), false)) {
            key = static_cast<UINT>(VK_F1 + current);
            capturing = false;
            return true;
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) capturing = false;
    return false;
}

void sectionButton(SettingsSection section, char const* label) {
    auto& page = state();
    bool const selected = page.section == section;
    EditorTheme const theme;
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button, theme.accent);
    if (ImGui::Button(label, {-1.0f, 38.0f})) page.section = section;
    if (selected) ImGui::PopStyleColor();
}

template <typename DrawControl>
void settingControlRow(char const* label, char const* description, DrawControl&& drawControl) {
    ImGui::PushID(label);
    float const tableWidth = std::min(ImGui::GetContentRegionAvail().x, 1220.0f);
    if (ImGui::BeginTable(
            "##setting-row",
            3,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
            {tableWidth, 0.0f}
        )) {
        ImGui::TableSetupColumn("##setting-label", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("##setting-description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##setting-control", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 72.0f);
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", description);
        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-1.0f);
        drawControl();
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void drawRecordingSection() {
    auto& page = state();
    auto& recorder = functions::Recorder::getInstance();
    ImGui::TextUnformatted("playback.settings.sections.recording"_tr().c_str());
    ImGui::TextDisabled("%s", "playback.settings.recording.description"_tr().c_str());
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("playback.settings.recording.shortcuts"_tr().c_str());
    settingControlRow(
        "playback.recording.settings.toggleRecording"_tr().c_str(),
        "playback.settings.recording.toggleRecordingDescription"_tr().c_str(),
        [&] {
            if (page.captureRecordingKey) {
                ImGui::Button("playback.recording.settings.pressKey"_tr().c_str(), {180.0f, 0.0f});
                captureKey(page.draft.toggleRecordingKey, page.captureRecordingKey);
            } else if (ImGui::Button(page.draft.keyDisplayName(page.draft.toggleRecordingKey).c_str(), {180.0f, 0.0f})) {
                page.captureRecordingKey = true;
                page.error.clear();
            }
        }
    );
    settingControlRow(
        "playback.recording.settings.togglePause"_tr().c_str(),
        "playback.settings.recording.togglePauseDescription"_tr().c_str(),
        [&] {
            if (page.capturePauseKey) {
                ImGui::Button("playback.recording.settings.pressKey"_tr().c_str(), {180.0f, 0.0f});
                captureKey(page.draft.togglePauseKey, page.capturePauseKey);
            } else if (ImGui::Button(page.draft.keyDisplayName(page.draft.togglePauseKey).c_str(), {180.0f, 0.0f})) {
                page.capturePauseKey = true;
                page.error.clear();
            }
        }
    );
    ImGui::Spacing();
    ImGui::TextUnformatted("playback.settings.recording.overlay"_tr().c_str());
    settingControlRow(
        "playback.recording.settings.showOverlay"_tr().c_str(),
        "playback.settings.recording.overlayDescription"_tr().c_str(),
        [&] { ImGui::Checkbox("##show-recording-overlay", &page.draft.showStatusOverlay); }
    );
    std::string const topLeft = "playback.recording.settings.position.topLeft"_tr();
    std::string const topRight = "playback.recording.settings.position.topRight"_tr();
    std::string const bottomLeft = "playback.recording.settings.position.bottomLeft"_tr();
    std::string const bottomRight = "playback.recording.settings.position.bottomRight"_tr();
    char const* positions[]{topLeft.c_str(), topRight.c_str(), bottomLeft.c_str(), bottomRight.c_str()};
    int position = static_cast<int>(page.draft.overlayPosition);
    settingControlRow(
        "playback.recording.settings.positionLabel"_tr().c_str(),
        "playback.settings.recording.positionDescription"_tr().c_str(),
        [&] {
            if (ImGui::Combo("##recording-overlay-position", &position, positions, IM_ARRAYSIZE(positions))) {
                page.draft.overlayPosition = static_cast<config::RecordingOverlayPosition>(position);
            }
        }
    );
    ImGui::Spacing();
    ImGui::TextUnformatted("playback.settings.recording.preview"_tr().c_str());
    ImGui::TextColored({0.92f, 0.22f, 0.22f, 1.0f}, "●");
    ImGui::SameLine();
    ImGui::TextUnformatted("playback.settings.recording.previewText"_tr().c_str());
    ImGui::Spacing();
    ImGui::TextUnformatted("playback.settings.recording.current"_tr().c_str());
    if (!recorder.isActive()) {
        if (ImGui::Button("playback.recording.controls.start"_tr().c_str(), {130.0f, 0.0f})) recorder.start();
    } else if (recorder.isPaused()) {
        if (ImGui::Button("playback.recording.controls.resume"_tr().c_str(), {130.0f, 0.0f})) recorder.start();
        ImGui::SameLine();
        if (ImGui::Button("playback.recording.controls.stop"_tr().c_str(), {130.0f, 0.0f})) recorder.stop();
    } else {
        if (ImGui::Button("playback.recording.controls.pause"_tr().c_str(), {130.0f, 0.0f})) recorder.pause();
        ImGui::SameLine();
        if (ImGui::Button("playback.recording.controls.stop"_tr().c_str(), {130.0f, 0.0f})) recorder.stop();
    }
}

void drawUnavailableSection(char const* title, char const* description) {
    ImGui::TextUnformatted(title);
    ImGui::TextDisabled("%s", description);
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("%s", "playback.settings.unavailable"_tr().c_str());
}

void drawCloseConfirmation() {
    auto& page = state();
    if (page.closeConfirmation) ImGui::OpenPopup("##settings-unsaved-changes");
    if (!ImGui::BeginPopupModal("##settings-unsaved-changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("playback.settings.unsavedChanges"_tr().c_str());
    ImGui::Spacing();
    if (ImGui::Button("playback.settings.actions.saveAndExit"_tr().c_str())) {
        std::string error;
        if (!page.draft.validate(&error)) {
            page.error = error == "duplicate_key" ? "playback.recording.settings.error.duplicate"_tr()
                                                   : "playback.recording.settings.error.unsupported"_tr();
            page.closeConfirmation = false;
            ImGui::CloseCurrentPopup();
        } else {
            auto config = Playback::getInstance().getConfig();
            config.recordingControls = page.draft;
            if (playback::config::save(config)) {
                Playback::getInstance().getConfig() = std::move(config);
                closeSettingsPage();
                ImGui::CloseCurrentPopup();
            } else {
                page.error = "playback.recording.settings.error.saveFailed"_tr();
                page.closeConfirmation = false;
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("playback.settings.actions.discardChanges"_tr().c_str())) {
        closeSettingsPage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("playback.settings.actions.keepEditing"_tr().c_str())) {
        page.closeConfirmation = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void openSettingsPage(SettingsSection section) {
    auto& page = state();
    page.open = true;
    page.section = section;
}

void closeSettingsPage() {
    auto& page = state();
    page.open = false;
    page.initialized = false;
    page.captureRecordingKey = false;
    page.capturePauseKey = false;
    page.closeConfirmation = false;
}

bool isSettingsPageOpen() { return state().open; }

void drawSettingsPage() {
    auto& page = state();
    if (!page.open) return;
    initializeDraft();
    auto& io = ImGui::GetIO();
    if (!page.captureRecordingKey && !page.capturePauseKey && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) requestClose();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {48.0f, 36.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {18.0f, 18.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14.0f, 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0f, 0.0f, 0.0f, 0.0f});
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin(
        "##playback-settings-overlay",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
    );
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImGui::GetWindowPos(),
        {ImGui::GetWindowPos().x + io.DisplaySize.x, ImGui::GetWindowPos().y + io.DisplaySize.y},
        IM_COL32(0, 0, 0, 248)
    );
    auto* bodyFont  = static_cast<ImFont*>(gFonts.body);
    auto* titleFont = static_cast<ImFont*>(gFonts.title);
    if (bodyFont) ImGui::PushFont(bodyFont);
    if (titleFont) {
        if (bodyFont) ImGui::PopFont();
        ImGui::PushFont(titleFont);
        ImGui::TextUnformatted("playback.settings.title"_tr().c_str());
        ImGui::PopFont();
        if (bodyFont) ImGui::PushFont(bodyFont);
    } else {
        ImGui::TextUnformatted("playback.settings.title"_tr().c_str());
    }
    ImGui::SameLine(io.DisplaySize.x - 140.0f);
    if (ImGui::Button("playback.settings.actions.close"_tr().c_str())) requestClose();
    ImGui::Separator();
    float const footerHeight = 78.0f;
    float const navigationWidth = 240.0f;
    ImGui::BeginChild(
        "##settings-navigation",
        {navigationWidth, -footerHeight},
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
    );
    sectionButton(SettingsSection::Recording, "playback.settings.sections.recording"_tr().c_str());
    sectionButton(SettingsSection::Browser, "playback.settings.sections.browser"_tr().c_str());
    sectionButton(SettingsSection::Editor, "playback.settings.sections.editor"_tr().c_str());
    sectionButton(SettingsSection::Export, "playback.settings.sections.export"_tr().c_str());
    sectionButton(SettingsSection::General, "playback.settings.sections.general"_tr().c_str());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild(
        "##settings-content",
        {0.0f, -footerHeight},
        ImGuiChildFlags_None,
        ImGuiWindowFlags_AlwaysVerticalScrollbar
    );
    switch (page.section) {
    case SettingsSection::Recording:
        drawRecordingSection();
        break;
    case SettingsSection::Browser:
        drawUnavailableSection("playback.settings.sections.browser"_tr().c_str(), "playback.settings.browser.description"_tr().c_str());
        break;
    case SettingsSection::Editor:
        drawUnavailableSection("playback.settings.sections.editor"_tr().c_str(), "playback.settings.editor.description"_tr().c_str());
        break;
    case SettingsSection::Export:
        drawUnavailableSection("playback.settings.sections.export"_tr().c_str(), "playback.settings.export.description"_tr().c_str());
        break;
    case SettingsSection::General:
        drawUnavailableSection("playback.settings.sections.general"_tr().c_str(), "playback.settings.general.description"_tr().c_str());
        break;
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (!page.error.empty()) ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "%s", page.error.c_str());
    float const footerButtonsWidth = 390.0f;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - footerButtonsWidth);
    if (ImGui::Button("playback.settings.actions.restoreDefaults"_tr().c_str())) {
        page.draft = config::RecordingControlsConfig::defaults();
        page.error.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("playback.settings.actions.cancel"_tr().c_str())) requestClose();
    ImGui::SameLine();
    if (ImGui::Button("playback.settings.actions.save"_tr().c_str())) {
        std::string error;
        if (!page.draft.validate(&error)) {
            page.error = error == "duplicate_key" ? "playback.recording.settings.error.duplicate"_tr()
                                                   : "playback.recording.settings.error.unsupported"_tr();
        } else {
            auto config = Playback::getInstance().getConfig();
            config.recordingControls = page.draft;
            if (playback::config::save(config)) {
                Playback::getInstance().getConfig() = std::move(config);
                closeSettingsPage();
            } else page.error = "playback.recording.settings.error.saveFailed"_tr();
        }
    }
    drawCloseConfirmation();
    if (bodyFont) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void setSettingsFonts(SettingsFonts fonts) { gFonts = fonts; }

} // namespace playback::editor::ui
