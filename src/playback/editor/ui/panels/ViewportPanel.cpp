#include "ViewportPanel.h"

#include "playback/editor/keyframe/CameraTimelineRegistry.h"
#include "playback/editor/ui/ReplayEditor.h"

#include "imgui.h"
#include "ll/api/i18n/I18n.h"
#include "ll/api/service/TargetedBedrock.h"

#include "mc/client/game/ClientInstance.h"
#include "mc/deps/renderer/Camera.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace playback::editor::ui {

using namespace ll::i18n_literals;

namespace {

constexpr float Pi               = 3.14159265358979323846f;
constexpr float RadiansPerDegree = Pi / 180.0f;
constexpr float DegreesPerRadian = 180.0f / Pi;

struct CameraBasis {
    ::glm::vec3 position;
    ::glm::vec3 right;
    ::glm::vec3 up;
    ::glm::vec3 forward;
    float       fov{};
    float       aspectRatio{};
};

struct CameraSpacePoint {
    float x{};
    float y{};
    float depth{};
};

float dot(::glm::vec3 const& left, ::glm::vec3 const& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

CameraSpacePoint toCameraSpace(CameraBasis const& camera, keyframe::CameraRenderState const& state) {
    ::glm::vec3 const delta{state.x - camera.position.x, state.y - camera.position.y, state.z - camera.position.z};
    return {dot(delta, camera.right), dot(delta, camera.up), dot(delta, camera.forward)};
}

std::optional<ImVec2> projectToViewport(CameraBasis const& camera, CameraSpacePoint point, Rect const& viewport) {
    constexpr float NearPlane = 0.025f;
    if (point.depth <= NearPlane) return std::nullopt;

    float const tangent = std::tan(std::clamp(camera.fov, 1.0f, 179.0f) * 0.5f * RadiansPerDegree);
    if (!std::isfinite(tangent) || tangent <= std::numeric_limits<float>::epsilon()) return std::nullopt;

    float const halfWidth  = (viewport.max.x - viewport.min.x) * 0.5f;
    float const halfHeight = (viewport.max.y - viewport.min.y) * 0.5f;
    float const ndcX       = std::clamp(point.x / (point.depth * tangent * camera.aspectRatio), -16.0f, 16.0f);
    float const ndcY       = std::clamp(point.y / (point.depth * tangent), -16.0f, 16.0f);
    return ImVec2{
        viewport.min.x + halfWidth * (1.0f + ndcX),
        viewport.min.y + halfHeight * (1.0f - ndcY),
    };
}

std::optional<CameraBasis> currentCameraBasis(Rect const& viewport) {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    auto const& camera   = client->getCamera();
    auto const  position = *camera.mPosition;
    auto const  right    = *camera.mRight;
    auto const  up       = *camera.mUp;
    auto const  forward  = *camera.mForward;
    float const width    = viewport.max.x - viewport.min.x;
    float const height   = viewport.max.y - viewport.min.y;
    float const aspect   = width > 0.0f && height > 0.0f ? width / height : 1.0f;
    float const fov      = camera.mFov;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(right.x) || !std::isfinite(right.y) || !std::isfinite(right.z) || !std::isfinite(up.x)
        || !std::isfinite(up.y) || !std::isfinite(up.z) || !std::isfinite(forward.x) || !std::isfinite(forward.y)
        || !std::isfinite(forward.z) || !std::isfinite(fov)) {
        return std::nullopt;
    }
    return CameraBasis{position, right, up, forward, fov, aspect};
}

std::optional<keyframe::CameraRenderState> captureCurrentCamera() {
    auto client = ll::service::getClientInstance();
    if (!client) return std::nullopt;

    auto const& camera   = client->getCamera();
    auto const  position = *camera.mPosition;
    auto const  forward  = *camera.mForward;
    float const fov      = camera.mFov;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)
        || !std::isfinite(forward.x) || !std::isfinite(forward.y) || !std::isfinite(forward.z)
        || !std::isfinite(fov)) {
        return std::nullopt;
    }

    return keyframe::CameraRenderState{
        position.x,
        position.y,
        position.z,
        std::atan2(-forward.x, forward.z) * DegreesPerRadian,
        std::atan2(-forward.y, std::hypot(forward.x, forward.z)) * DegreesPerRadian,
        0.0f,
        std::clamp(fov, 1.0f, 179.0f),
    };
}

void cameraVectors(float yaw, float pitch, ::glm::vec3& forward, ::glm::vec3& right, ::glm::vec3& up) {
    float const yawRadians   = yaw * RadiansPerDegree;
    float const pitchRadians = pitch * RadiansPerDegree;
    float const cosPitch     = std::cos(pitchRadians);
    float const sinPitch     = std::sin(pitchRadians);
    float const sinYaw       = std::sin(yawRadians);
    float const cosYaw       = std::cos(yawRadians);
    forward                  = {-sinYaw * cosPitch, -sinPitch, cosYaw * cosPitch};
    right                    = {cosYaw, 0.0f, sinYaw};
    up                       = {-sinYaw * sinPitch, cosPitch, cosYaw * sinPitch};
}

} // namespace

void ViewportPanel::draw(bool maximized) {
    ImVec2 viewportSize = ImGui::GetContentRegionAvail();

    constexpr float kTransportHeight = 42.0f;
    ImVec2          sceneSize        = viewportSize;
    if (maximized) sceneSize.y = std::max(1.0f, sceneSize.y - kTransportHeight);
    ImDrawList* dl       = ImGui::GetWindowDrawList();
    ImVec2      sceneMin = ImGui::GetCursorScreenPos();
    ImVec2      sceneMax = ImVec2(sceneMin.x + sceneSize.x, sceneMin.y + sceneSize.y);
    dl->AddRectFilled(sceneMin, sceneMax, IM_COL32(0x0d, 0x0d, 0x0d, 0xff));
    float  sceneAspectRatio = sceneSize.x / std::max(1.0f, sceneSize.y);
    ImVec2 videoSize = sceneAspectRatio > mVideoAspectRatio ? ImVec2(sceneSize.y * mVideoAspectRatio, sceneSize.y)
                                                            : ImVec2(sceneSize.x, sceneSize.x / mVideoAspectRatio);
    ImVec2 videoMin(sceneMin.x + (sceneSize.x - videoSize.x) * 0.5f, sceneMin.y + (sceneSize.y - videoSize.y) * 0.5f);
    ImVec2 videoMax(videoMin.x + videoSize.x, videoMin.y + videoSize.y);
    mVideoRect = {videoMin, videoMax};
    if (mGameTexture) {
        dl->AddImage(ImTextureRef(mGameTexture), videoMin, videoMax);
    }
    dl->AddRect(videoMin, videoMax, IM_COL32(0x3a, 0x8c, 0xf0, 0xff));
    ImGui::SetCursorScreenPos(videoMin);
    ImGui::InvisibleButton("##viewport-video", videoSize);
    bool        videoHovered = ImGui::IsItemHovered();
    bool        videoActive  = ImGui::IsItemActive();
    auto const& state        = ReplayEditor::getInstance().state();
    mContextMenu.draw(state.capabilities.cameraEditing);

    if (state.capabilities.cameraEditing) {
        drawCameraPathOverlay(*dl);
    }

    constexpr float kMaximizeButtonSize = 28.0f;
    ImVec2          maximizePos(sceneMax.x - kMaximizeButtonSize - 8.0f, sceneMax.y - kMaximizeButtonSize - 8.0f);
    ImGui::SetCursorScreenPos(maximizePos);
    ImGui::InvisibleButton("##viewport-maximize", {kMaximizeButtonSize, kMaximizeButtonSize});
    bool        maximizeHovered = ImGui::IsItemHovered();
    ImDrawList* overlay         = ImGui::GetWindowDrawList();
    overlay->AddRectFilled(
        maximizePos,
        {maximizePos.x + kMaximizeButtonSize, maximizePos.y + kMaximizeButtonSize},
        maximizeHovered ? IM_COL32(58, 90, 140, 235) : IM_COL32(20, 20, 24, 210),
        4.0f
    );
    ImU32 iconColor = IM_COL32(230, 232, 238, 255);
    float x = maximizePos.x, y = maximizePos.y;
    if (maximized) {
        overlay->AddLine({x + 8, y + 12}, {x + 8, y + 8}, iconColor, 1.8f);
        overlay->AddLine({x + 8, y + 8}, {x + 12, y + 8}, iconColor, 1.8f);
        overlay->AddLine({x + 20, y + 16}, {x + 20, y + 20}, iconColor, 1.8f);
        overlay->AddLine({x + 20, y + 20}, {x + 16, y + 20}, iconColor, 1.8f);
        overlay->AddLine({x + 16, y + 8}, {x + 20, y + 8}, iconColor, 1.8f);
        overlay->AddLine({x + 20, y + 8}, {x + 20, y + 12}, iconColor, 1.8f);
        overlay->AddLine({x + 12, y + 20}, {x + 8, y + 20}, iconColor, 1.8f);
        overlay->AddLine({x + 8, y + 20}, {x + 8, y + 16}, iconColor, 1.8f);
    } else {
        overlay->AddLine({x + 7, y + 12}, {x + 7, y + 7}, iconColor, 1.8f);
        overlay->AddLine({x + 7, y + 7}, {x + 12, y + 7}, iconColor, 1.8f);
        overlay->AddLine({x + 21, y + 16}, {x + 21, y + 21}, iconColor, 1.8f);
        overlay->AddLine({x + 21, y + 21}, {x + 16, y + 21}, iconColor, 1.8f);
        overlay->AddLine({x + 16, y + 7}, {x + 21, y + 7}, iconColor, 1.8f);
        overlay->AddLine({x + 21, y + 7}, {x + 21, y + 12}, iconColor, 1.8f);
        overlay->AddLine({x + 12, y + 21}, {x + 7, y + 21}, iconColor, 1.8f);
        overlay->AddLine({x + 7, y + 21}, {x + 7, y + 16}, iconColor, 1.8f);
    }
    if (ImGui::IsItemClicked()) ReplayEditor::getInstance().toggleViewportMaximized();
    if (maximizeHovered)
        ImGui::SetTooltip(
            "%s",
            (maximized ? "playback.refactorEditor.timeline.restore"_tr()
                       : "playback.refactorEditor.timeline.maximize"_tr())
                .c_str()
        );
    if (state.capabilities.cameraEditing) {
        handleCameraControl(videoHovered && !maximizeHovered, videoActive && !maximizeHovered);
    }
    if (maximized) drawTransportControls();
}

void ViewportPanel::drawTransportControls() {
    auto&           editor        = ReplayEditor::getInstance();
    auto const&     state         = editor.state();
    ImVec2          available     = ImGui::GetContentRegionAvail();
    ImVec2          origin        = ImGui::GetCursorScreenPos();
    constexpr float buttonSize    = 32.0f;
    constexpr float gap           = 8.0f;
    constexpr float controlsWidth = buttonSize * 5.0f + gap * 4.0f;
    float           startX        = origin.x + (available.x - controlsWidth) * 0.5f;
    float           y             = origin.y + 5.0f;
    auto            button        = [y](const char* id, float x, auto drawIcon) {
        ImGui::SetCursorScreenPos({x, y});
        ImGui::InvisibleButton(id, {buttonSize, buttonSize});
        ImU32 color = ImGui::IsItemHovered() ? IM_COL32(240, 192, 32, 255) : IM_COL32(230, 232, 238, 255);
        drawIcon(ImGui::GetWindowDrawList(), ImVec2(x + buttonSize * 0.5f, y + buttonSize * 0.5f), color);
        return ImGui::IsItemClicked();
    };
    if (button("##viewport-start", startX, [](ImDrawList* dl, ImVec2 c, ImU32 color) {
            dl->AddLine({c.x - 9, c.y - 8}, {c.x - 9, c.y + 8}, color, 2);
            dl->AddTriangleFilled({c.x - 7, c.y}, {c.x + 7, c.y - 8}, {c.x + 7, c.y + 8}, color);
        })) {
        editor.seekTo(0);
    }
    if (button("##viewport-back", startX + (buttonSize + gap), [](ImDrawList* dl, ImVec2 c, ImU32 color) {
            dl->AddTriangleFilled({c.x - 9, c.y}, {c.x + 5, c.y - 8}, {c.x + 5, c.y + 8}, color);
            dl->AddTriangleFilled({c.x - 2, c.y}, {c.x + 10, c.y - 8}, {c.x + 10, c.y + 8}, color);
        })) {
        editor.seekRelative(-200);
    }
    if (button("##viewport-play", startX + (buttonSize + gap) * 2, [&state](ImDrawList* dl, ImVec2 c, ImU32 color) {
            if (!state.paused) {
                dl->AddRectFilled({c.x - 7, c.y - 8}, {c.x - 2, c.y + 8}, color);
                dl->AddRectFilled({c.x + 2, c.y - 8}, {c.x + 7, c.y + 8}, color);
            } else dl->AddTriangleFilled({c.x - 6, c.y - 9}, {c.x - 6, c.y + 9}, {c.x + 9, c.y}, color);
        })) {
        editor.submitAction({playback::editor::EditorActionType::TogglePause});
    }
    if (button("##viewport-forward", startX + (buttonSize + gap) * 3, [](ImDrawList* dl, ImVec2 c, ImU32 color) {
            dl->AddTriangleFilled({c.x - 10, c.y - 8}, {c.x - 10, c.y + 8}, {c.x + 2, c.y}, color);
            dl->AddTriangleFilled({c.x - 3, c.y - 8}, {c.x - 3, c.y + 8}, {c.x + 9, c.y}, color);
        })) {
        editor.seekRelative(200);
    }
    if (button("##viewport-end", startX + (buttonSize + gap) * 4, [](ImDrawList* dl, ImVec2 c, ImU32 color) {
            dl->AddTriangleFilled({c.x - 7, c.y - 8}, {c.x - 7, c.y + 8}, {c.x + 7, c.y}, color);
            dl->AddLine({c.x + 9, c.y - 8}, {c.x + 9, c.y + 8}, color, 2);
        })) {
        editor.seekTo(state.totalTicks);
    }
}

void ViewportPanel::setGameTexture(ImTextureID texture) { mGameTexture = texture; }

void ViewportPanel::setVideoAspectRatio(float aspectRatio) { mVideoAspectRatio = std::max(0.1f, aspectRatio); }

void ViewportPanel::resetCameraControl() {
    mViewportCamera.reset();
    mViewportCameraTick = -1;
    keyframe::clearPreviewCameraOverride();
}

void ViewportPanel::handleCameraControl(bool hovered, bool active) {
    auto const& editorState = ReplayEditor::getInstance().state();
    if (!editorState.paused) {
        resetCameraControl();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    bool const rotate = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    bool const pan    = hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right);
    bool const dolly  = hovered && std::abs(io.MouseWheel) > std::numeric_limits<float>::epsilon();
    bool const keyboard = hovered && !io.WantTextInput
                       && (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_A)
                           || ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_D)
                           || ImGui::IsKeyDown(ImGuiKey_Q) || ImGui::IsKeyDown(ImGuiKey_E));
    bool const controlling = rotate || pan || dolly || keyboard;

    if (mViewportCamera && mViewportCameraTick != editorState.currentTick && !controlling) {
        resetCameraControl();
    }
    if (!controlling) return;

    if (!mViewportCamera) mViewportCamera = captureCurrentCamera();
    if (!mViewportCamera) return;
    mViewportCameraTick = editorState.currentTick;

    auto& camera = *mViewportCamera;
    if (rotate) {
        camera.yaw += io.MouseDelta.x * 0.20f;
        camera.pitch = std::clamp(camera.pitch + io.MouseDelta.y * 0.20f, -89.9f, 89.9f);
    }

    ::glm::vec3 forward;
    ::glm::vec3 right;
    ::glm::vec3 up;
    cameraVectors(camera.yaw, camera.pitch, forward, right, up);
    auto translate = [&](::glm::vec3 const& direction, float amount) {
        camera.x += direction.x * amount;
        camera.y += direction.y * amount;
        camera.z += direction.z * amount;
    };
    if (pan) {
        translate(right, -io.MouseDelta.x * 0.045f);
        translate(up, io.MouseDelta.y * 0.045f);
    }
    if (dolly) translate(forward, io.MouseWheel * 2.0f);
    if (keyboard) {
        float const speed = (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift))
                          ? 24.0f
                          : 8.0f;
        float const distance = speed * std::clamp(io.DeltaTime, 0.0f, 0.1f);
        if (ImGui::IsKeyDown(ImGuiKey_W)) translate(forward, distance);
        if (ImGui::IsKeyDown(ImGuiKey_S)) translate(forward, -distance);
        if (ImGui::IsKeyDown(ImGuiKey_D)) translate(right, distance);
        if (ImGui::IsKeyDown(ImGuiKey_A)) translate(right, -distance);
        if (ImGui::IsKeyDown(ImGuiKey_E)) translate(up, distance);
        if (ImGui::IsKeyDown(ImGuiKey_Q)) translate(up, -distance);
    }
    keyframe::publishPreviewCameraOverride(camera);
}

void ViewportPanel::drawCameraPathOverlay(ImDrawList& drawList) {
    auto const& state = ReplayEditor::getInstance().state();
    if (!state.project || state.totalTicks <= 0 || exporting::isExportActive(state.exportStatus.state)) return;

    auto const camera = currentCameraBasis(mVideoRect);
    if (!camera) return;

    constexpr size_t MaxPathSamples = 600;
    auto const path = keyframe::sampleCameraTimelineRange(
        keyframe::CameraTimelineSource::Preview,
        0,
        state.totalTicks,
        MaxPathSamples
    );
    if (path.empty()) return;

    constexpr float NearPlane = 0.025f;
    drawList.PushClipRect(mVideoRect.min, mVideoRect.max, true);
    for (size_t index = 1; index < path.size(); ++index) {
        auto previous = toCameraSpace(*camera, path[index - 1]);
        auto current  = toCameraSpace(*camera, path[index]);
        if (previous.depth <= NearPlane && current.depth <= NearPlane) continue;
        if (previous.depth <= NearPlane || current.depth <= NearPlane) {
            float const amount = (NearPlane - previous.depth) / (current.depth - previous.depth);
            CameraSpacePoint const clipped{
                previous.x + (current.x - previous.x) * amount,
                previous.y + (current.y - previous.y) * amount,
                NearPlane,
            };
            if (previous.depth <= NearPlane) previous = clipped;
            else current = clipped;
        }
        auto const start = projectToViewport(*camera, previous, mVideoRect);
        auto const end   = projectToViewport(*camera, current, mVideoRect);
        if (!start || !end) continue;
        drawList.AddLine(*start, *end, IM_COL32(12, 12, 14, 210), 4.0f);
        drawList.AddLine(*start, *end, IM_COL32(246, 196, 48, 245), 2.0f);
    }

    std::string_view selectedCameraId;
    auto const&      selection = ReplayEditor::getInstance().selection();
    if (auto const* selectedCamera = selection.getAs<editing::model::SelectedCamera>()) {
        selectedCameraId = selectedCamera->cameraId;
    } else if (auto const* selectedKeyframe = selection.getAs<editing::model::SelectedKeyframe>()) {
        selectedCameraId = selectedKeyframe->trackId;
    }
    for (auto const& projectCamera : state.project->cameras) {
        if (!selectedCameraId.empty() && projectCamera.id != selectedCameraId) continue;
        for (auto const& key : projectCamera.keys) {
            keyframe::CameraRenderState const keyState{
                key.position.x,
                key.position.y,
                key.position.z,
                key.yaw,
                key.pitch,
                0.0f,
                key.fov,
            };
            auto const projected = projectToViewport(*camera, toCameraSpace(*camera, keyState), mVideoRect);
            if (!projected) continue;
            drawList.AddCircleFilled(*projected, 4.5f, IM_COL32(18, 18, 20, 235), 12);
            drawList.AddCircleFilled(*projected, 2.75f, IM_COL32(255, 220, 98, 255), 12);
        }
    }

    auto const time = functions::render::ReplaySampleTime::fromRational(state.currentTick, 1);
    if (time) {
        auto const current = keyframe::sampleCameraTimeline(keyframe::CameraTimelineSource::Preview, *time);
        if (current) {
            auto const projected = projectToViewport(*camera, toCameraSpace(*camera, current->state), mVideoRect);
            if (projected) {
                drawList.AddCircleFilled(*projected, 6.0f, IM_COL32(14, 18, 16, 235), 16);
                drawList.AddCircleFilled(*projected, 3.5f, IM_COL32(74, 222, 128, 255), 16);
            }
        }
    }
    drawList.PopClipRect();
}

} // namespace playback::editor::ui
