#include "ClipOps.h"

#include "ll/api/i18n/I18n.h"
#include "playback/state/editing/CameraBindingOps.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace playback::state::editing::ClipOps {

using namespace ll::i18n_literals;

namespace {

bool clipIdExists(model::EditorStateExt const& state, std::string_view id) noexcept {
    for (auto const& cam : state.cameras) {
        if (std::ranges::any_of(cam.clips, [&](auto const& c) { return c.id == id; })) return true;
    }
    for (auto const& scene : state.scenes) {
        for (auto const& cam : scene.cameras) {
            if (std::ranges::any_of(cam.clips, [&](auto const& c) { return c.id == id; })) return true;
        }
    }
    return false;
}

bool clipNameExists(model::EditorStateExt const& state, std::string_view name) noexcept {
    for (auto const& cam : state.cameras) {
        if (std::ranges::any_of(cam.clips, [&](auto const& c) { return c.name == name; })) return true;
    }
    for (auto const& scene : state.scenes) {
        for (auto const& cam : scene.cameras) {
            if (std::ranges::any_of(cam.clips, [&](auto const& c) { return c.name == name; })) return true;
        }
    }
    return false;
}

bool audioTrackIdExists(model::EditorStateExt const& state, std::string_view id) noexcept {
    if (std::ranges::any_of(state.audioTracks, [&](auto const& t) { return t.id == id; })) return true;
    for (auto const& scene : state.scenes) {
        if (std::ranges::any_of(scene.audioTracks, [&](auto const& t) { return t.id == id; })) return true;
    }
    return false;
}

bool audioTrackNameExists(model::EditorStateExt const& state, std::string_view name) noexcept {
    return std::ranges::any_of(state.audioTracks, [&](auto const& t) { return t.name == name; });
}

bool audioClipIdExists(model::EditorStateExt const& state, std::string_view id) noexcept {
    for (auto const& track : state.audioTracks) {
        if (std::ranges::any_of(track.clips, [&](auto const& c) { return c.id == id; })) return true;
    }
    for (auto const& scene : state.scenes) {
        for (auto const& track : scene.audioTracks) {
            if (std::ranges::any_of(track.clips, [&](auto const& c) { return c.id == id; })) return true;
        }
    }
    return false;
}

void clampExportRange(int& exportInTick, int& exportOutTick, int totalTicks) noexcept {
    if (exportInTick >= 0) exportInTick = std::clamp(exportInTick, 0, totalTicks);
    if (exportOutTick >= 0) exportOutTick = std::clamp(exportOutTick, 0, totalTicks);
    if (exportInTick >= 0 && exportOutTick >= 0 && exportInTick >= exportOutTick) {
        exportInTick  = -1;
        exportOutTick = -1;
    }
}

void syncCamerasForTotalTicks(
    model::EditorStateExt&            state,
    std::vector<model::CameraEntity>& cameras,
    int&                              exportInTick,
    int&                              exportOutTick,
    int                               previousTotalTicks,
    std::string const&                preferredClipName
) {
    int const totalTicks = state.totalTicks;
    if (previousTotalTicks <= 0 && totalTicks > 0) {
        for (auto& camera : cameras) {
            if (camera.clips.empty()) {
                model::Clip clip;
                clip.id = nextClipId(state);
                if (!preferredClipName.empty() && !clipNameExists(state, preferredClipName)) {
                    clip.name = preferredClipName;
                } else {
                    clip.name = defaultClipName(state);
                }
                clip.inTick        = 0;
                clip.outTick       = totalTicks;
                clip.exportEnabled = true;
                camera.clips.push_back(std::move(clip));
            } else if (camera.clips.size() == 1 && camera.clips[0].inTick == 0 && camera.clips[0].outTick <= 1) {
                camera.clips[0].outTick = totalTicks;
            }
        }
    }

    if (totalTicks <= 0) return;
    for (auto& camera : cameras) {
        for (auto& clip : camera.clips) {
            clip.inTick  = std::clamp(clip.inTick, 0, totalTicks);
            clip.outTick = std::clamp(clip.outTick, clip.inTick, totalTicks);
        }
        std::ranges::stable_sort(camera.clips, {}, &model::Clip::inTick);
    }
    clampExportRange(exportInTick, exportOutTick, totalTicks);
}

model::Clip const*
findCoveringClip(model::CameraEntity const& camera, int tick, int totalTicks, bool onlyExportEnabled) noexcept {
    for (auto const& clip : camera.clips) {
        if ((!onlyExportEnabled || clip.exportEnabled) && clipCoversTick(clip, tick, totalTicks)) {
            return &clip;
        }
    }
    return nullptr;
}

model::CameraEntity const* findCameraById(model::EditorStateExt const& state, std::string_view cameraId) noexcept {
    if (cameraId.empty()) return nullptr;
    for (auto const& camera : state.cameras) {
        if (camera.id == cameraId) return &camera;
    }
    return nullptr;
}

template <typename Dst, typename Src>
void copySceneScopeFields(Dst& dst, Src const& src) {
    dst.cameras        = src.cameras;
    dst.markers        = src.markers;
    dst.speedTrack             = src.speedTrack;
    dst.freezeTrack            = src.freezeTrack;
    dst.timelapseTrack         = src.timelapseTrack;
    dst.timeOfDayTrack         = src.timeOfDayTrack;
    dst.weatherTrack           = src.weatherTrack;
    dst.daytimeOverrideEnabled = src.daytimeOverrideEnabled;
    dst.daytimeOverrideTicks   = src.daytimeOverrideTicks;
    dst.weatherOverride        = src.weatherOverride;
    dst.audioTracks            = src.audioTracks;
    dst.exportInTick   = src.exportInTick;
    dst.exportOutTick  = src.exportOutTick;
}

} // namespace

bool clipCoversTick(model::Clip const& clip, int tick, int totalTicks) noexcept {
    return totalTicks > 0 && tick >= 0 && clip.inTick < clip.outTick
        && ((clip.inTick <= tick && tick < clip.outTick) || (tick == totalTicks && clip.outTick == totalTicks));
}

bool cameraHasActiveClipAt(
    model::CameraEntity const& camera,
    int                        tick,
    int                        totalTicks,
    bool                       onlyExportEnabled
) noexcept {
    if (!camera.enabled) return false;
    return findCoveringClip(camera, tick, totalTicks, onlyExportEnabled) != nullptr;
}

std::string nextClipId(model::EditorStateExt const& state) {
    for (size_t next = 1;; ++next) {
        auto id = "clip_" + std::to_string(next);
        if (!clipIdExists(state, id)) return id;
    }
}

std::string defaultClipName(model::EditorStateExt const& state) {
    for (size_t next = 1;; ++next) {
        auto candidate = "playback.refactorEditor.defaults.clip"_tr(next);
        if (!clipNameExists(state, candidate)) return candidate;
    }
}

std::string nextSceneId(model::EditorStateExt const& state) {
    for (size_t next = state.scenes.size() + 1;; ++next) {
        auto id = "scene_" + std::to_string(next);
        if (std::ranges::none_of(state.scenes, [&id](auto const& scene) { return scene.id == id; })) {
            return id;
        }
    }
}

std::string nextAudioTrackId(model::EditorStateExt const& state) {
    for (size_t next = 1;; ++next) {
        auto id = "audio_" + std::to_string(next);
        if (!audioTrackIdExists(state, id)) return id;
    }
}

std::string defaultAudioTrackName(model::EditorStateExt const& state) {
    for (size_t next = 1;; ++next) {
        auto candidate = "playback.refactorEditor.audio.defaultTrackName"_tr(next);
        if (candidate == "playback.refactorEditor.audio.defaultTrackName" || candidate.empty()) {
            candidate = "Audio " + std::to_string(next);
        }
        if (!audioTrackNameExists(state, candidate)) return candidate;
    }
}

std::string nextAudioClipId(model::EditorStateExt const& state) {
    for (size_t next = 1;; ++next) {
        auto id = "aclip_" + std::to_string(next);
        if (!audioClipIdExists(state, id)) return id;
    }
}

model::AudioTrack const* findAudioTrack(model::EditorStateExt const& state, std::string_view trackId) noexcept {
    if (trackId.empty()) return nullptr;
    for (auto const& track : state.audioTracks) {
        if (track.id == trackId) return &track;
    }
    return nullptr;
}

model::AudioTrack* findAudioTrack(model::EditorStateExt& state, std::string_view trackId) noexcept {
    if (trackId.empty()) return nullptr;
    for (auto& track : state.audioTracks) {
        if (track.id == trackId) return &track;
    }
    return nullptr;
}

model::AudioClip const* findAudioClip(model::AudioTrack const& track, std::string_view clipId) noexcept {
    if (clipId.empty()) return nullptr;
    for (auto const& clip : track.clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

model::AudioClip* findAudioClip(model::AudioTrack& track, std::string_view clipId) noexcept {
    if (clipId.empty()) return nullptr;
    for (auto& clip : track.clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

model::Scene const* findScene(model::EditorStateExt const& state, std::string_view sceneId) noexcept {
    if (sceneId.empty()) return nullptr;
    for (auto const& scene : state.scenes) {
        if (scene.id == sceneId) return &scene;
    }
    return nullptr;
}

model::Scene* findScene(model::EditorStateExt& state, std::string_view sceneId) noexcept {
    if (sceneId.empty()) return nullptr;
    for (auto& scene : state.scenes) {
        if (scene.id == sceneId) return &scene;
    }
    return nullptr;
}

model::Scene const* activeScene(model::EditorStateExt const& state) noexcept {
    if (auto const* scene = findScene(state, state.activeSceneId)) return scene;
    return state.scenes.empty() ? nullptr : &state.scenes.front();
}

model::Scene* activeScene(model::EditorStateExt& state) noexcept {
    if (auto* scene = findScene(state, state.activeSceneId)) return scene;
    return state.scenes.empty() ? nullptr : &state.scenes.front();
}

void syncActiveSceneFromWorking(model::EditorStateExt& state) {
    auto* scene = activeScene(state);
    if (!scene) return;
    if (state.activeSceneId != scene->id) state.activeSceneId = scene->id;
    copySceneScopeFields(*scene, state);
}

void loadWorkingFromActiveScene(model::EditorStateExt& state) {
    auto const* scene = activeScene(state);
    if (!scene) return;
    if (state.activeSceneId != scene->id) state.activeSceneId = scene->id;
    copySceneScopeFields(state, *scene);
}

model::EditorStateExt buildProjectForScene(model::EditorStateExt const& state, model::Scene const& scene) {
    model::EditorStateExt result;
    result.projectName      = state.projectName;
    result.projectPath      = state.projectPath;
    result.currentTick      = state.currentTick;
    result.totalTicks       = state.totalTicks;
    result.playing          = state.playing;
    result.playbackSpeed    = state.playbackSpeed;
    result.activeSceneId    = scene.id;
    result.transitions      = state.transitions;
    result.fps              = state.fps;
    result.memoryUsageBytes = state.memoryUsageBytes;

    auto const* active   = activeScene(state);
    bool const  isActive = active ? (&scene == active || (!scene.id.empty() && scene.id == active->id))
                                  : (!scene.id.empty() && scene.id == state.activeSceneId);
    if (isActive) {
        copySceneScopeFields(result, state);
    } else {
        copySceneScopeFields(result, scene);
    }
    return result;
}

void ensureDefaultScene(model::EditorStateExt& state, std::string defaultSceneName, std::string preferredClipName) {
    if (state.scenes.empty()) {
        if (state.cameras.empty()) CameraBindingOps::addFreeCamera(state, {});

        model::Scene scene;
        scene.id            = "scene_1";
        scene.name          = defaultSceneName.empty() ? "playback.refactorEditor.defaults.scene"_tr(size_t{1})
                                                       : std::move(defaultSceneName);
        copySceneScopeFields(scene, state);

        state.scenes.push_back(std::move(scene));
        state.activeSceneId = "scene_1";

        if (state.totalTicks > 0) {
            auto& created = state.scenes.back();
            for (size_t i = 0; i < created.cameras.size(); ++i) {
                auto& cam = created.cameras[i];
                if (!cam.clips.empty()) continue;
                model::Clip clip;
                clip.id = nextClipId(state);
                if (i == 0 && !preferredClipName.empty() && !clipNameExists(state, preferredClipName)) {
                    clip.name = preferredClipName;
                } else {
                    clip.name = defaultClipName(state);
                }
                clip.inTick        = 0;
                clip.outTick       = state.totalTicks;
                clip.exportEnabled = true;
                cam.clips.push_back(std::move(clip));
            }
        }

        loadWorkingFromActiveScene(state);
        return;
    }

    for (auto& scene : state.scenes) {
        if (scene.id.empty()) scene.id = nextSceneId(state);
        if (scene.cameras.empty()) {
            model::EditorStateExt temp;
            CameraBindingOps::addFreeCamera(temp, {});
            scene.cameras = std::move(temp.cameras);
            if (state.totalTicks > 0 && !scene.cameras.empty()) {
                model::Clip clip;
                clip.id            = nextClipId(state);
                clip.name          = defaultClipName(state);
                clip.inTick        = 0;
                clip.outTick       = state.totalTicks;
                clip.exportEnabled = true;
                scene.cameras.front().clips.push_back(std::move(clip));
            }
        }
    }

    auto const* current = findScene(state, state.activeSceneId);
    if (!current) {
        state.activeSceneId = state.scenes.front().id;
        loadWorkingFromActiveScene(state);
    } else if (state.cameras.empty()) {
        loadWorkingFromActiveScene(state);
    }
}

void syncTotalTicks(model::EditorStateExt& state, int previousTotalTicks, std::string preferredClipName) {
    if (!state.scenes.empty()) {
        syncActiveSceneFromWorking(state);
        for (auto& scene : state.scenes) {
            syncCamerasForTotalTicks(
                state,
                scene.cameras,
                scene.exportInTick,
                scene.exportOutTick,
                previousTotalTicks,
                preferredClipName
            );
        }
        loadWorkingFromActiveScene(state);
        return;
    }

    syncCamerasForTotalTicks(
        state,
        state.cameras,
        state.exportInTick,
        state.exportOutTick,
        previousTotalTicks,
        preferredClipName
    );
}

ClipWithCamera activeClipAt(
    model::EditorStateExt const& state,
    int                          tick,
    bool                         onlyExportEnabled,
    std::span<int const>         dimensionTransitions
) noexcept {
    auto dimensionSegment = [&](int t) -> std::ptrdiff_t {
        return std::upper_bound(dimensionTransitions.begin(), dimensionTransitions.end(), t)
             - dimensionTransitions.begin();
    };

    // Pass 1: highest-priority enabled camera with both an active keyframe shot and a covering clip.
    for (auto const& camera : state.cameras) {
        if (!camera.enabled || camera.keysByTick.empty()) continue;
        auto const* clip = findCoveringClip(camera, tick, state.totalTicks, onlyExportEnabled);
        if (!clip) continue;
        auto const upper = camera.keysByTick.upper_bound(tick);
        if (upper == camera.keysByTick.begin()) continue;
        auto const& [keyTick, key] = *std::prev(upper);
        if (key.mode == model::CameraKeyframeMode::Interpolated && !dimensionTransitions.empty()
            && dimensionSegment(keyTick) != dimensionSegment(tick)) {
            continue;
        }
        return {&camera, clip};
    }

    // Pass 2: fallback to the highest-priority enabled camera that has a covering clip.
    for (auto const& camera : state.cameras) {
        if (!camera.enabled) continue;
        if (auto const* clip = findCoveringClip(camera, tick, state.totalTicks, onlyExportEnabled)) {
            return {&camera, clip};
        }
    }

    return {};
}

std::vector<CompositedClipSlice> collectCompositedSlices(
    model::EditorStateExt const& state,
    bool                         onlyExportEnabled,
    std::span<int const>         dimensionTransitions
) {
    if (state.totalTicks <= 0) return {};

    std::vector<int> boundaries;
    boundaries.push_back(0);
    boundaries.push_back(state.totalTicks);
    for (auto const& camera : state.cameras) {
        if (!camera.enabled) continue;
        for (auto const& clip : camera.clips) {
            boundaries.push_back(std::clamp(clip.inTick, 0, state.totalTicks));
            boundaries.push_back(std::clamp(clip.outTick, 0, state.totalTicks));
        }
        for (auto const& [kTick, _] : camera.keysByTick) {
            boundaries.push_back(std::clamp(kTick, 0, state.totalTicks));
        }
    }
    for (int t : dimensionTransitions) {
        boundaries.push_back(std::clamp(t, 0, state.totalTicks));
    }

    std::ranges::sort(boundaries);
    auto [uniqueEnd, end] = std::ranges::unique(boundaries);
    boundaries.erase(uniqueEnd, end);

    std::vector<CompositedClipSlice> slices;
    for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
        int const b0 = boundaries[i];
        int const b1 = boundaries[i + 1];
        if (b0 >= b1) continue;
        auto const hit = activeClipAt(state, b0, onlyExportEnabled, dimensionTransitions);
        if (!hit) continue;
        if (!slices.empty() && slices.back().outTick == b0 && slices.back().clip == hit.clip) {
            slices.back().outTick = b1;
        } else {
            slices.push_back(CompositedClipSlice{b0, b1, hit.camera, hit.clip, hit.clip->exportEnabled});
        }
    }
    return slices;
}

std::vector<ClipWithCamera> collectAllClipsWithCamera(model::EditorStateExt const& state) {
    std::vector<ClipWithCamera> result;
    for (auto const& camera : state.cameras) {
        std::vector<model::Clip const*> camClips;
        camClips.reserve(camera.clips.size());
        for (auto const& clip : camera.clips) camClips.push_back(&clip);
        std::ranges::stable_sort(camClips, [](model::Clip const* a, model::Clip const* b) {
            return a->inTick < b->inTick;
        });
        for (auto const* clip : camClips) result.push_back({&camera, clip});
    }
    return result;
}

std::vector<model::Clip const*> collectOrderedClips(model::EditorStateExt const& state) {
    auto withCamera = collectAllClipsWithCamera(state);
    std::ranges::stable_sort(withCamera, [](ClipWithCamera const& a, ClipWithCamera const& b) {
        return a.clip->inTick < b.clip->inTick;
    });
    std::vector<model::Clip const*> result;
    result.reserve(withCamera.size());
    for (auto const& entry : withCamera) result.push_back(entry.clip);
    return result;
}

std::vector<model::Clip const*> collectEnabledClips(model::EditorStateExt const& state) {
    auto withCamera = collectAllClipsWithCamera(state);
    std::erase_if(withCamera, [](ClipWithCamera const& entry) {
        return !entry.camera || !entry.camera->enabled || !entry.clip || !entry.clip->exportEnabled
            || entry.clip->inTick >= entry.clip->outTick;
    });
    std::ranges::stable_sort(withCamera, [](ClipWithCamera const& a, ClipWithCamera const& b) {
        return a.clip->inTick < b.clip->inTick;
    });
    std::vector<model::Clip const*> result;
    result.reserve(withCamera.size());
    for (auto const& entry : withCamera) result.push_back(entry.clip);
    return result;
}

ClipWithCamera findClipWithCamera(model::EditorStateExt const& state, std::string_view clipId) noexcept {
    if (clipId.empty()) return {};
    for (auto const& camera : state.cameras) {
        for (auto const& clip : camera.clips) {
            if (clip.id == clipId) return {&camera, &clip};
        }
    }
    return {};
}

MutableClipWithCamera findClipWithCamera(model::EditorStateExt& state, std::string_view clipId) noexcept {
    if (clipId.empty()) return {};
    for (auto& camera : state.cameras) {
        for (auto& clip : camera.clips) {
            if (clip.id == clipId) return {&camera, &clip};
        }
    }
    return {};
}

model::Clip const* findClipById(model::EditorStateExt const& state, std::string_view clipId) noexcept {
    return findClipWithCamera(state, clipId).clip;
}

model::Clip* findClipById(model::EditorStateExt& state, std::string_view clipId) noexcept {
    return findClipWithCamera(state, clipId).clip;
}

std::vector<model::SelectedClipCut> collectCameraClipCuts(model::CameraEntity const& camera, int totalTicks) {
    if (totalTicks <= 0) return {};

    struct Interval {
        int inTick{};
        int outTick{};
    };
    std::vector<Interval> intervals;
    intervals.reserve(camera.clips.size());
    for (auto const& clip : camera.clips) {
        int const cIn  = std::clamp(clip.inTick, 0, totalTicks);
        int const cOut = std::clamp(clip.outTick, 0, totalTicks);
        if (cIn < cOut) intervals.push_back({cIn, cOut});
    }
    if (intervals.empty()) {
        return {
            model::SelectedClipCut{camera.id, 0, totalTicks}
        };
    }

    std::ranges::sort(intervals, [](Interval const& a, Interval const& b) {
        return a.inTick != b.inTick ? a.inTick < b.inTick : a.outTick < b.outTick;
    });

    std::vector<model::SelectedClipCut> cuts;
    int                                 cursor = 0;
    for (auto const& iv : intervals) {
        if (iv.inTick > cursor) {
            cuts.push_back({camera.id, cursor, iv.inTick});
        } else if (iv.inTick == cursor && cursor > 0) {
            model::SelectedClipCut seam{camera.id, cursor, cursor};
            if (cuts.empty() || cuts.back() != seam) cuts.push_back(std::move(seam));
        }
        cursor = std::max(cursor, iv.outTick);
    }
    if (cursor < totalTicks) {
        cuts.push_back({camera.id, cursor, totalTicks});
    }
    return cuts;
}

model::CameraEntity const*
selectedCamera(model::EditorStateExt const& state, model::SelectionModel const& selection) noexcept {
    if (auto const* selected = selection.getAs<model::SelectedCamera>()) {
        return findCameraById(state, selected->cameraId);
    }
    if (auto const* selected = selection.getAs<model::SelectedClip>()) {
        return findClipWithCamera(state, selected->clipId).camera;
    }
    if (auto const* selected = selection.getAs<model::SelectedKeyframe>()) {
        return findCameraById(state, selected->trackId);
    }
    if (auto const* selected = selection.getAs<model::SelectedClipCut>()) {
        return findCameraById(state, selected->cameraId);
    }
    return nullptr;
}

std::string resolveKeyframeTargetCamera(model::EditorStateExt const& state, model::SelectionModel const& selection) {
    if (auto const* camera = selectedCamera(state, selection)) {
        return camera->locked ? std::string{} : camera->id;
    }
    for (auto const& camera : state.cameras) {
        if (camera.enabled && !camera.locked) return camera.id;
    }
    for (auto const& camera : state.cameras) {
        if (!camera.locked) return camera.id;
    }
    return {};
}

ClipWithCamera resolveExportToggleClip(
    model::EditorStateExt const& state,
    model::SelectionModel const& selection,
    int                          tick,
    int                          totalTicks,
    std::span<int const>         dimensionTransitions
) noexcept {
    if (auto const* selectedClip = selection.getAs<model::SelectedClip>()) {
        return findClipWithCamera(state, selectedClip->clipId);
    }
    if (auto const* camera = selectedCamera(state, selection)) {
        if (auto const* clip = findCoveringClip(*camera, tick, totalTicks, false)) {
            return {camera, clip};
        }
        return {};
    }
    if (auto const active = activeClipAt(state, tick, false, dimensionTransitions); active && !active.camera->locked) {
        return active;
    }
    for (auto const& camera : state.cameras) {
        if (camera.locked) continue;
        if (auto const* clip = findCoveringClip(camera, tick, totalTicks, false)) {
            return {&camera, clip};
        }
    }
    for (auto const& camera : state.cameras) {
        if (!camera.locked) continue;
        if (auto const* clip = findCoveringClip(camera, tick, totalTicks, false)) {
            return {&camera, clip};
        }
    }
    return {};
}

} // namespace playback::state::editing::ClipOps
