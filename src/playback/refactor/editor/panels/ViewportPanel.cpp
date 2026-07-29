#include "ViewportPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

namespace playback::refactor::editor {

void ViewportPanel::draw() {
    ImGui::Begin("Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    // Floating toolbar overlay (32px height at top of viewport)
    {
        ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos());
        ImGui::BeginChild("##viewportToolbar", ImVec2(viewportSize.x, 32.0f), false,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        if (ImGui::Button(ICON_PLAY " Play")) {
            Editor::getInstance().state().playing = !Editor::getInstance().state().playing;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_ADD_KEYFRAME " Add Keyframe")) {
            // Placeholder: add keyframe at playhead
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_ADD_MARKER " Add Marker")) {
            // Placeholder: add marker
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_EXPORT " Export")) {
            ModeManager::getInstance().switchTo(EditorMode::Render);
        }

        // Tooltips on hover
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Export (Ctrl+E)");
        }

        ImGui::EndChild();
    }

    // 3D scene area (placeholder: gradient background)
    ImVec2 sceneSize = ImVec2(viewportSize.x, ImGui::GetContentRegionAvail().y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 sceneMin = ImGui::GetCursorScreenPos();
    ImVec2 sceneMax = ImVec2(sceneMin.x + sceneSize.x, sceneMin.y + sceneSize.y);
    dl->AddRectFilled(sceneMin, sceneMax, IM_COL32(0x0d, 0x0d, 0x0d, 0xff));

    // Handle camera controls (orbit/pan/dolly) in empty area
    handleCameraControl();

    // Draw gizmo if a keyframe is selected
    auto* sel = Editor::getInstance().selection().getSelection();
    if (sel) {
        if (auto* kfSel = std::get_if<SelectedKeyframe>(sel)) {
            drawGizmo();
        }
    }

    ImGui::Dummy(sceneSize);
    ImGui::End();
}

void ViewportPanel::handleCameraControl() {
    // No selection: orbit/pan/dolly on mouse drag
    if (!Editor::getInstance().selection().hasSelection()) {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            mViewportRotation.x += io.MouseDelta.y * 0.5f;
            mViewportRotation.y += io.MouseDelta.x * 0.5f;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            mViewportAnchor.x += io.MouseDelta.x * 0.1f;
            mViewportAnchor.z += io.MouseDelta.y * 0.1f;
        }
        if (io.MouseWheel != 0.0f) {
            mViewportAnchor.y += io.MouseWheel * 5.0f;
        }
    }
}

void ViewportPanel::handleGizmoDrag() {
    // Placeholder: ImGuizmo integration
    // ImGuizmo::Manipulate(viewMatrix, projMatrix, TRANSLATE, LOCAL, glm::value_ptr(newPos), nullptr, nullptr);
}

void ViewportPanel::drawGizmo() {
    // Placeholder: draw ImGuizmo 3-axis arrow + rotation rings
    // This will be implemented with ImGuizmo::Manipulate when ImGuizmo is available
}

} // namespace playback::refactor::editor