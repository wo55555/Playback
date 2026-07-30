#include "MenuBar.h"

#include "Editor.h"
#include "EditorBridge.h"
#include "iconfont.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

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
            if (ImGui::MenuItem("Keyboard Shortcuts")) mShortcutDialogOpen = true;
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

    if (mShortcutDialogOpen) ImGui::OpenPopup("Keyboard Shortcuts");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Keyboard Shortcuts", &mShortcutDialogOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        ImGui::TextUnformatted("Playback"); ImGui::Separator();
        ImGui::BulletText("Space: Play / Pause");
        ImGui::BulletText("Home / End: Jump to start / end");
        ImGui::BulletText("Left / Right: Seek one frame");
        ImGui::BulletText("Shift + Mouse Wheel: Zoom timeline");
        ImGui::Spacing(); ImGui::TextUnformatted("Editing"); ImGui::Separator();
        ImGui::BulletText("Ctrl + Z / Ctrl + Y: Undo / Redo");
        ImGui::BulletText("K: Add camera keyframe");
        ImGui::BulletText("M: Insert marker");
        ImGui::BulletText("Delete: Delete selected item");
        ImGui::Spacing(); ImGui::TextUnformatted("Application"); ImGui::Separator();
        ImGui::BulletText("Ctrl + S: Save project");
        ImGui::BulletText("Esc (hold): Exit editor");
        ImGui::Spacing();
        if (ImGui::Button("Close", {110.0f, 32.0f})) { mShortcutDialogOpen = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    if (mExportDialogOpen) ImGui::OpenPopup("Export Video");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(660.0f, 650.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Export Video", &mExportDialogOpen, ImGuiWindowFlags_NoResize)) {
        constexpr const char* aspectOptions[] = {"16:9  Widescreen", "9:16  Vertical", "1:1  Square", "Custom"};
        constexpr const char* resolutionOptions[] = {"1920 x 1080  Full HD", "1280 x 720  HD", "2560 x 1440  QHD", "3840 x 2160  4K", "Custom"};
        constexpr const char* fpsOptions[] = {"30 FPS", "60 FPS", "120 FPS", "Custom"};
        constexpr const char* bitrateOptions[] = {"10 Mbps  Compact", "20 Mbps  Balanced", "40 Mbps  High quality", "Custom"};
        constexpr const char* formatOptions[] = {"MP4", "MKV", "WebM", "MOV", "AVI"};
        constexpr const char* codecOptions[][5] = {{"H.264", "H.265 / HEVC", nullptr}, {"H.264", "H.265 / HEVC", "VP9", "AV1", nullptr}, {"VP9", "AV1", nullptr}, {"H.264", "H.265 / HEVC", "ProRes", nullptr}, {"H.264", "MJPEG", nullptr}};
        constexpr int codecCounts[] = {2, 4, 2, 3, 2};
        constexpr int widths[] = {1920, 1280, 2560, 3840};
        constexpr int heights[] = {1080, 720, 1440, 2160};
        constexpr int fpsValues[] = {30, 60, 120};
        constexpr int bitrateValues[] = {10, 20, 40};

        ImGui::TextUnformatted("Export Video");
        ImGui::SameLine();
        ImGui::TextDisabled("Configure output settings");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("OUTPUT");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Name"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(-1.0f); ImGui::InputText("##export-name", mExportName.data(), mExportName.size());
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Location"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(-1.0f); ImGui::InputText("##export-directory", mExportDirectory.data(), mExportDirectory.size());
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Format"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-format", &mFormatPreset, formatOptions, IM_ARRAYSIZE(formatOptions))) mCodecPreset = 0;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Video codec"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f); ImGui::Combo("##export-codec", &mCodecPreset, codecOptions[mFormatPreset], codecCounts[mFormatPreset]);

        ImGui::Spacing();
        ImGui::TextUnformatted("VIDEO");
        ImGui::Separator();
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Aspect ratio"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-aspect", &mAspectPreset, aspectOptions, IM_ARRAYSIZE(aspectOptions)) && mAspectPreset != 3) {
            float aspect = mAspectPreset == 0 ? 16.0f / 9.0f : mAspectPreset == 1 ? 9.0f / 16.0f : 1.0f;
            mHeight = std::max(1, static_cast<int>(mWidth / aspect + 0.5f));
        }
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Resolution"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-resolution", &mResolutionPreset, resolutionOptions, IM_ARRAYSIZE(resolutionOptions)) && mResolutionPreset < 4) {
            mWidth = widths[mResolutionPreset];
            mHeight = heights[mResolutionPreset];
            if (mAspectPreset == 1) std::swap(mWidth, mHeight);
            if (mAspectPreset == 2) mHeight = mWidth;
        }
        if (mResolutionPreset == 4) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(92.0f); ImGui::InputInt("##export-width", &mWidth);
            ImGui::SameLine(); ImGui::TextUnformatted("x"); ImGui::SameLine(); ImGui::SetNextItemWidth(92.0f); ImGui::InputInt("##export-height", &mHeight);
            mWidth = std::clamp(mWidth, 16, 7680); mHeight = std::clamp(mHeight, 16, 4320);
        }
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Frame rate"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-fps", &mFpsPreset, fpsOptions, IM_ARRAYSIZE(fpsOptions)) && mFpsPreset < 3) mFps = fpsValues[mFpsPreset];
        if (mFpsPreset == 3) { ImGui::SameLine(); ImGui::SetNextItemWidth(92.0f); ImGui::InputInt("##export-custom-fps", &mFps); mFps = std::clamp(mFps, 1, 240); }
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Bitrate"); ImGui::SameLine(150.0f);
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::Combo("##export-bitrate", &mBitratePreset, bitrateOptions, IM_ARRAYSIZE(bitrateOptions)) && mBitratePreset < 3) mBitrateMbps = bitrateValues[mBitratePreset];
        if (mBitratePreset == 3) { ImGui::SameLine(); ImGui::SetNextItemWidth(92.0f); ImGui::InputInt("##export-custom-bitrate", &mBitrateMbps); ImGui::SameLine(); ImGui::TextUnformatted("Mbps"); mBitrateMbps = std::clamp(mBitrateMbps, 1, 200); }

        ImGui::Spacing();
        ImGui::Separator();
        char summary[192];
        const char* extensions[] = {"mp4", "mkv", "webm", "mov", "avi"};
        std::snprintf(summary, sizeof(summary), "%dx%d  |  %d FPS  |  %d Mbps  |  %s / %s", mWidth, mHeight, mFps, mBitrateMbps, formatOptions[mFormatPreset], codecOptions[mFormatPreset][mCodecPreset]);
        ImGui::TextDisabled("OUTPUT SUMMARY"); ImGui::SameLine(); ImGui::TextUnformatted(summary);
        ImGui::TextDisabled("File: %s/%s.%s", mExportDirectory.data(), mExportName.data(), extensions[mFormatPreset]);
        ImGui::Spacing();
        ImGui::BeginDisabled();
        ImGui::Button("Export Video", ImVec2(140.0f, 32.0f));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Export backend is not available yet");
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110.0f, 32.0f))) {
            mExportDialogOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

bool MenuBar::isAnyMenuOpen() const {
    return ImGui::IsAnyItemHovered();
}

} // namespace playback::refactor::editor
