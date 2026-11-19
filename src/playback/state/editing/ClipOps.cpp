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
                clip.speed         = 1.0f;
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
    scene->cameras       = state.cameras;
    scene->markers       = state.markers;
    scene->exportInTick  = state.exportInTick;
    scene->exportOutTick = state.exportOutTick;
}

void loadWorkingFromActiveScene(model::EditorStateExt& state) {
    auto const* scene = activeScene(state);
    if (!scene) return;
    if (state.activeSceneId != scene->id) state.activeSceneId = scene->id;
    state.cameras       = scene->cameras;
    state.markers       = scene->markers;
    state.exportInTick  = scene->exportInTick;
    state.exportOutTick = scene->exportOutTick;
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
        result.cameras       = state.cameras;
        result.markers       = state.markers;
        result.exportInTick  = state.exportInTick;
        result.exportOutTick = state.exportOutTick;
    } else {
        result.cameras       = scene.cameras;
        result.markers       = scene.markers;
        result.exportInTick  = scene.exportInTick;
        result.exportOutTick = scene.exportOutTick;
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
        scene.cameras       = state.cameras;
        scene.markers       = state.markers;
        scene.exportInTick  = state.exportInTick;
        scene.exportOutTick = state.exportOutTick;

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
                clip.speed         = 1.0f;
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
                clip.speed         = 1.0f;
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

float activeClipSpeedAt(model::EditorStateExt const& state, int tick) noexcept {
    if (auto const hit = activeClipAt(state, tick, false)) {
        return model::clampClipSpeed(hit.clip->speed);
    }
    return 1.0f;
}

bool isClipMutedAt(model::EditorStateExt const& state, int tick) noexcept {
    if (auto const hit = activeClipAt(state, tick, false)) {
        return hit.clip->muted;
    }
    return false;
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
            slices.push_back(
                CompositedClipSlice{
                    b0,
                    b1,
                    hit.camera,
                    hit.clip,
                    model::clampClipSpeed(hit.clip->speed),
                    hit.clip->muted,
                    hit.clip->exportEnabled
                }
            );
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

} // namespace playback::state::editing::ClipOps
