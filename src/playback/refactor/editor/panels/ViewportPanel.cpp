#include "ViewportPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/iconfont.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"

#include <algorithm>

namespace playback::refactor::editor {

void ViewportPanel::draw() {
    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    ImVec2 sceneSize = viewportSize;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 sceneMin = ImGui::GetCursorScreenPos();
    ImVec2 sceneMax = ImVec2(sceneMin.x + sceneSize.x, sceneMin.y + sceneSize.y);
    dl->AddRectFilled(sceneMin, sceneMax, IM_COL32(0x0d, 0x0d, 0x0d, 0xff));
    float sceneAspectRatio = sceneSize.x / std::max(1.0f, sceneSize.y);
    ImVec2 videoSize = sceneAspectRatio > mVideoAspectRatio
        ? ImVec2(sceneSize.y * mVideoAspectRatio, sceneSize.y)
        : ImVec2(sceneSize.x, sceneSize.x / mVideoAspectRatio);
    ImVec2 videoMin(
        sceneMin.x + (sceneSize.x - videoSize.x) * 0.5f,
        sceneMin.y + (sceneSize.y - videoSize.y) * 0.5f);
    ImVec2 videoMax(videoMin.x + videoSize.x, videoMin.y + videoSize.y);
    mVideoRect = {videoMin, videoMax};
    if (mGameTexture) {
        dl->AddImage(ImTextureRef(mGameTexture), videoMin, videoMax);
    }
    dl->AddRect(videoMin, videoMax, IM_COL32(0x3a, 0x8c, 0xf0, 0xff));
    ImGui::SetCursorScreenPos(videoMin);
    ImGui::InvisibleButton("##viewport-video", videoSize);
    bool videoHovered = ImGui::IsItemHovered();
    bool videoActive = ImGui::IsItemActive();
    mContextMenu.draw();

    ImGui::SetCursorScreenPos(videoMin);
    ImGui::BeginChild("##viewportToolbar", ImVec2(videoSize.x, 32.0f), false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground);
    if (ImGui::Button(Editor::getInstance().state().playing ? ICON_PAUSE " Pause" : ICON_PLAY " Play")) {
        EditorBridge::getInstance().playPause();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD_KEYFRAME " Add Keyframe")) {
        auto& state = Editor::getInstance().state();
        for (auto& track : state.cameraTracks) {
            if (track.active) {
                EditorBridge::getInstance().addKeyframe(state, track.id, state.currentTick);
                break;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_ADD_MARKER " Add Marker")) {
        auto& state = Editor::getInstance().state();
        EditorBridge::getInstance().addMarker(state, "Marker", state.currentTick);
    }
    ImGui::EndChild();

    handleCameraControl(videoHovered, videoActive);

    auto* sel = Editor::getInstance().selection().getSelection();
    if (sel) {
        if (auto* kfSel = std::get_if<SelectedKeyframe>(sel)) {
            drawGizmo();
        }
    }

}

void ViewportPanel::setGameTexture(ImTextureID texture) {
    mGameTexture = texture;
}

void ViewportPanel::setVideoAspectRatio(float aspectRatio) {
    mVideoAspectRatio = std::max(0.1f, aspectRatio);
}

void ViewportPanel::handleCameraControl(bool hovered, bool active) {
    // No selection: orbit/pan/dolly on mouse drag
    if (!Editor::getInstance().selection().hasSelection()) {
        ImGuiIO& io = ImGui::GetIO();

        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            mViewportRotation.x += io.MouseDelta.y * 0.5f;
            mViewportRotation.y += io.MouseDelta.x * 0.5f;
        }
        if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            mViewportAnchor.x += io.MouseDelta.x * 0.1f;
            mViewportAnchor.z += io.MouseDelta.y * 0.1f;
        }
        if (hovered && io.MouseWheel != 0.0f) {
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
