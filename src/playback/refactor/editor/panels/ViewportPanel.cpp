#include "ViewportPanel.h"

#include "playback/refactor/editor/Editor.h"
#include "playback/refactor/editor/EditorBridge.h"
#include "playback/refactor/editor/models/SelectionModel.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"

#include <algorithm>

namespace playback::refactor::editor {

void ViewportPanel::draw(bool maximized) {
    using ll::i18n_literals::operator""_tr;
    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    constexpr float kTransportHeight = 42.0f;
    ImVec2 sceneSize = viewportSize;
    if (maximized) sceneSize.y = std::max(1.0f, sceneSize.y - kTransportHeight);
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

    handleCameraControl(videoHovered, videoActive);

    constexpr float kMaximizeButtonSize = 28.0f;
    ImVec2 maximizePos(videoMax.x - kMaximizeButtonSize - 8.0f, videoMax.y - kMaximizeButtonSize - 8.0f);
    ImGui::SetCursorScreenPos(maximizePos);
    ImGui::InvisibleButton("##viewport-maximize", {kMaximizeButtonSize, kMaximizeButtonSize});
    bool maximizeHovered = ImGui::IsItemHovered();
    ImDrawList* overlay = ImGui::GetWindowDrawList();
    overlay->AddRectFilled(maximizePos, {maximizePos.x + kMaximizeButtonSize, maximizePos.y + kMaximizeButtonSize}, maximizeHovered ? IM_COL32(58, 90, 140, 235) : IM_COL32(20, 20, 24, 210), 4.0f);
    ImU32 iconColor = IM_COL32(230, 232, 238, 255);
    if (maximized) {
        overlay->AddRect({maximizePos.x + 8, maximizePos.y + 8}, {maximizePos.x + 18, maximizePos.y + 18}, iconColor, 0.0f, 0, 1.5f);
        overlay->AddLine({maximizePos.x + 6, maximizePos.y + 11}, {maximizePos.x + 11, maximizePos.y + 11}, iconColor, 1.5f);
        overlay->AddLine({maximizePos.x + 11, maximizePos.y + 6}, {maximizePos.x + 11, maximizePos.y + 11}, iconColor, 1.5f);
    } else {
        overlay->AddRect({maximizePos.x + 7, maximizePos.y + 7}, {maximizePos.x + 21, maximizePos.y + 21}, iconColor, 0.0f, 0, 1.5f);
    }
    if (ImGui::IsItemClicked()) Editor::getInstance().toggleViewportMaximized();
    if (maximizeHovered) ImGui::SetTooltip("%s", (maximized ? "playback.refactorEditor.timeline.restore"_tr() : "playback.refactorEditor.timeline.maximize"_tr()).c_str());

    auto* sel = Editor::getInstance().selection().getSelection();
    if (sel) {
        if (auto* kfSel = std::get_if<SelectedKeyframe>(sel)) {
            drawGizmo();
        }
    }
    if (maximized) drawTransportControls();
}

void ViewportPanel::drawTransportControls() {
    auto& bridge = EditorBridge::getInstance();
    auto& state = Editor::getInstance().state();
    ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    constexpr float buttonSize = 32.0f;
    constexpr float gap = 8.0f;
    constexpr float controlsWidth = buttonSize * 5.0f + gap * 4.0f;
    float startX = origin.x + (available.x - controlsWidth) * 0.5f;
    float y = origin.y + 5.0f;
    auto button = [y](const char* id, float x, auto drawIcon) {
        ImGui::SetCursorScreenPos({x, y});
        ImGui::InvisibleButton(id, {buttonSize, buttonSize});
        ImU32 color = ImGui::IsItemHovered() ? IM_COL32(240, 192, 32, 255) : IM_COL32(230, 232, 238, 255);
        drawIcon(ImGui::GetWindowDrawList(), ImVec2(x + buttonSize * 0.5f, y + buttonSize * 0.5f), color);
        return ImGui::IsItemClicked();
    };
    if (button("##viewport-start", startX, [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddLine({c.x - 9, c.y - 8}, {c.x - 9, c.y + 8}, color, 2); dl->AddTriangleFilled({c.x - 7, c.y}, {c.x + 7, c.y - 8}, {c.x + 7, c.y + 8}, color); })) bridge.skipToStart();
    if (button("##viewport-back", startX + (buttonSize + gap), [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 9, c.y}, {c.x + 5, c.y - 8}, {c.x + 5, c.y + 8}, color); dl->AddTriangleFilled({c.x - 2, c.y}, {c.x + 10, c.y - 8}, {c.x + 10, c.y + 8}, color); })) bridge.seek(std::max(0, state.currentTick - 200));
    if (button("##viewport-play", startX + (buttonSize + gap) * 2, [&state](ImDrawList* dl, ImVec2 c, ImU32 color) { if (state.playing) { dl->AddRectFilled({c.x - 7, c.y - 8}, {c.x - 2, c.y + 8}, color); dl->AddRectFilled({c.x + 2, c.y - 8}, {c.x + 7, c.y + 8}, color); } else dl->AddTriangleFilled({c.x - 6, c.y - 9}, {c.x - 6, c.y + 9}, {c.x + 9, c.y}, color); })) bridge.playPause();
    if (button("##viewport-forward", startX + (buttonSize + gap) * 3, [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 10, c.y - 8}, {c.x - 10, c.y + 8}, {c.x + 2, c.y}, color); dl->AddTriangleFilled({c.x - 3, c.y - 8}, {c.x - 3, c.y + 8}, {c.x + 9, c.y}, color); })) bridge.seek(std::min(state.totalTicks, state.currentTick + 200));
    if (button("##viewport-end", startX + (buttonSize + gap) * 4, [](ImDrawList* dl, ImVec2 c, ImU32 color) { dl->AddTriangleFilled({c.x - 7, c.y - 8}, {c.x - 7, c.y + 8}, {c.x + 7, c.y}, color); dl->AddLine({c.x + 9, c.y - 8}, {c.x + 9, c.y + 8}, color, 2); })) bridge.skipToEnd();
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
