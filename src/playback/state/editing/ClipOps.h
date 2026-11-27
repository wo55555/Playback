#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::ClipOps {

struct ClipWithCamera {
    model::CameraEntity const* camera{};
    model::Clip const*         clip{};

    [[nodiscard]] explicit operator bool() const noexcept { return camera != nullptr && clip != nullptr; }
};

struct MutableClipWithCamera {
    model::CameraEntity* camera{};
    model::Clip*         clip{};

    [[nodiscard]] explicit operator bool() const noexcept { return camera != nullptr && clip != nullptr; }
};

struct CompositedClipSlice {
    int                        inTick{};
    int                        outTick{};
    model::CameraEntity const* camera{};
    model::Clip const*         clip{};
    bool                       exportEnabled{true};
};

[[nodiscard]] bool clipCoversTick(model::Clip const& clip, int tick, int totalTicks) noexcept;
[[nodiscard]] bool cameraHasActiveClipAt(
    model::CameraEntity const& camera,
    int                        tick,
    int                        totalTicks,
    bool                       onlyExportEnabled = false
) noexcept;

[[nodiscard]] std::string nextClipId(model::EditorStateExt const& state);
[[nodiscard]] std::string defaultClipName(model::EditorStateExt const& state);
[[nodiscard]] std::string nextSceneId(model::EditorStateExt const& state);
[[nodiscard]] std::string nextAudioTrackId(model::EditorStateExt const& state);
[[nodiscard]] std::string defaultAudioTrackName(model::EditorStateExt const& state);
[[nodiscard]] std::string nextAudioClipId(model::EditorStateExt const& state);

[[nodiscard]] model::AudioTrack const*
                                 findAudioTrack(model::EditorStateExt const& state, std::string_view trackId) noexcept;
[[nodiscard]] model::AudioTrack* findAudioTrack(model::EditorStateExt& state, std::string_view trackId) noexcept;
[[nodiscard]] model::AudioClip const* findAudioClip(model::AudioTrack const& track, std::string_view clipId) noexcept;
[[nodiscard]] model::AudioClip*       findAudioClip(model::AudioTrack& track, std::string_view clipId) noexcept;

[[nodiscard]] model::Scene const* findScene(model::EditorStateExt const& state, std::string_view sceneId) noexcept;
[[nodiscard]] model::Scene*       findScene(model::EditorStateExt& state, std::string_view sceneId) noexcept;
[[nodiscard]] model::Scene const* activeScene(model::EditorStateExt const& state) noexcept;
[[nodiscard]] model::Scene*       activeScene(model::EditorStateExt& state) noexcept;

void syncActiveSceneFromWorking(model::EditorStateExt& state);
void loadWorkingFromActiveScene(model::EditorStateExt& state);

[[nodiscard]] model::EditorStateExt buildProjectForScene(model::EditorStateExt const& state, model::Scene const& scene);

void ensureDefaultScene(
    model::EditorStateExt& state,
    std::string            defaultSceneName = {},
    std::string            defaultClipName  = {}
);

void syncTotalTicks(model::EditorStateExt& state, int previousTotalTicks, std::string defaultClipName = {});

[[nodiscard]] ClipWithCamera activeClipAt(
    model::EditorStateExt const& state,
    int                          tick,
    bool                         onlyExportEnabled    = false,
    std::span<int const>         dimensionTransitions = {}
) noexcept;

[[nodiscard]] std::vector<CompositedClipSlice> collectCompositedSlices(
    model::EditorStateExt const& state,
    bool                         onlyExportEnabled    = true,
    std::span<int const>         dimensionTransitions = {}
);

[[nodiscard]] std::vector<ClipWithCamera>     collectAllClipsWithCamera(model::EditorStateExt const& state);
[[nodiscard]] std::vector<model::Clip const*> collectOrderedClips(model::EditorStateExt const& state);
[[nodiscard]] std::vector<model::Clip const*> collectEnabledClips(model::EditorStateExt const& state);
[[nodiscard]] ClipWithCamera findClipWithCamera(model::EditorStateExt const& state, std::string_view clipId) noexcept;
[[nodiscard]] MutableClipWithCamera findClipWithCamera(model::EditorStateExt& state, std::string_view clipId) noexcept;
[[nodiscard]] model::Clip const*    findClipById(model::EditorStateExt const& state, std::string_view clipId) noexcept;
[[nodiscard]] model::Clip*          findClipById(model::EditorStateExt& state, std::string_view clipId) noexcept;

[[nodiscard]] std::vector<model::SelectedClipCut>
collectCameraClipCuts(model::CameraEntity const& camera, int totalTicks);

[[nodiscard]] model::CameraEntity const*
selectedCamera(model::EditorStateExt const& state, model::SelectionModel const& selection) noexcept;

[[nodiscard]] std::string
resolveKeyframeTargetCamera(model::EditorStateExt const& state, model::SelectionModel const& selection);

[[nodiscard]] ClipWithCamera resolveExportToggleClip(
    model::EditorStateExt const& state,
    model::SelectionModel const& selection,
    int                          tick,
    int                          totalTicks,
    std::span<int const>         dimensionTransitions = {}
) noexcept;

} // namespace playback::state::editing::ClipOps
