#pragma once

namespace playback::editor::keyframe {
struct CameraTimelineSample;
}

namespace playback::editor::renderer {

// Applies the sampled camera pose to the replay player for one complete native
// rendering pass and restores every touched component on destruction.
class ScopedNativeCameraPose {
public:
    explicit ScopedNativeCameraPose(keyframe::CameraTimelineSample const& sample);
    ~ScopedNativeCameraPose();

    ScopedNativeCameraPose(ScopedNativeCameraPose const&)            = delete;
    ScopedNativeCameraPose& operator=(ScopedNativeCameraPose const&) = delete;
    ScopedNativeCameraPose(ScopedNativeCameraPose&& other) noexcept;
    ScopedNativeCameraPose& operator=(ScopedNativeCameraPose&& other) noexcept;

    [[nodiscard]] bool applied() const noexcept { return mState != nullptr; }

private:
    void* mState{};
};

// Installs the final render-stage camera override at both GameRenderer's
// frame boundary and LevelRendererPlayer's native camera setup boundary.
// Preview and export therefore consume the same immutable timeline sample.
[[nodiscard]] bool hookCameraRender(bool enable);
[[nodiscard]] bool isCameraRenderInstalled();

} // namespace playback::editor::renderer
