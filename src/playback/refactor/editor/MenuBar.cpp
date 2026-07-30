#include "MenuBar.h"

#include "Editor.h"
#include "EditorBridge.h"
#include "ModeManager.h"
#include "iconfont.h"

#include "imgui.h"

namespace playback::refactor::editor {

void MenuBar::draw() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Replay", "Ctrl+O")) {}
            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {}
            if (ImGui::BeginMenu("Recent")) {
                for (int i = 0; i < 10; ++i) {
                    ImGui::MenuItem("(empty)", nullptr, false, false);
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Export...")) {
                mExportDialogOpen = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit Editor", "Esc (hold)")) {
                Editor::getInstance().toggle();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            auto& bridge = EditorBridge::getInstance();
            bool canUndo = bridge.canUndo();
            bool canRedo = bridge.canRedo();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
                bridge.undo(Editor::getInstance().state());
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo)) {
                bridge.redo(Editor::getInstance().state());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del", false, Editor::getInstance().selection().hasSelection())) {
                // Placeholder: delete selected
            }
            if (ImGui::MenuItem("Select All", "Ctrl+A")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Camera")) {
            if (ImGui::MenuItem("Add Keyframe at Playhead", "K")) {}
            if (ImGui::MenuItem("Add Camera Track", "Ctrl+Shift+N")) {}
            if (ImGui::BeginMenu("Camera Preset")) {
                ImGui::MenuItem("First Person");
                ImGui::MenuItem("Third Person");
                ImGui::MenuItem("Free");
                ImGui::MenuItem("Follow Entity");
                ImGui::MenuItem("Orbit");
                ImGui::MenuItem("Telephoto");
                ImGui::MenuItem("Drone");
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Set Active Camera 1", "1")) {}
            if (ImGui::MenuItem("Set Active Camera 2", "2")) {}
            if (ImGui::MenuItem("Set Active Camera 3", "3")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Markers")) {
            if (ImGui::MenuItem("Insert Marker", "M")) {}
            if (ImGui::MenuItem("Jump to Next Marker", "]")) {}
            if (ImGui::MenuItem("Jump to Previous Marker", "[")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Toggle Hint Bar", "F1")) {
                // HintBar toggle handled via Editor
            }
            ImGui::Separator();
            bool curveOpen = Editor::getInstance().curveEditorPanel().isOpen();
            if (ImGui::MenuItem("Curve Editor", nullptr, &curveOpen)) {
                Editor::getInstance().curveEditorPanel().setOpen(curveOpen);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Documentation")) {
                // Open documentation link
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About")) {
                // Show about dialog
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    if (mExportDialogOpen) ImGui::OpenPopup("Export");
    if (ImGui::BeginPopupModal("Export", &mExportDialogOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Export configuration");
        ImGui::Separator();
        ImGui::TextUnformatted("Format: MP4 (H.264)");
        ImGui::TextUnformatted("Resolution: 1920 x 1080");
        ImGui::TextUnformatted("FPS: 60");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f))) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Start", ImVec2(120.0f, 0.0f))) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
            ModeManager::getInstance().switchTo(EditorMode::Render);
        }
        ImGui::EndPopup();
    }
}

bool MenuBar::isAnyMenuOpen() const {
    return ImGui::IsAnyItemHovered();
}

} // namespace playback::refactor::editor
