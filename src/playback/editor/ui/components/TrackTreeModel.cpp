#include "TrackTreeModel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <cctype>

namespace playback::editor::ui {

using namespace playback::state::editing::model;

namespace {

bool containsInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(
               value.begin(),
               value.end(),
               query.begin(),
               query.end(),
               [](char left, char right) {
                   return std::tolower(static_cast<unsigned char>(left))
                       == std::tolower(static_cast<unsigned char>(right));
               }
           )
        != value.end();
}

bool cameraMatchesSearch(const CameraEntity& camera, const WorldActor& worldActor, std::string_view query) {
    if (containsInsensitive(camera.name, query)) return true;
    auto subActor =
        std::find_if(worldActor.subActors.begin(), worldActor.subActors.end(), [&camera](const SubActor& actor) {
            return actor.id == camera.bindingEntityUuid;
        });
    return subActor != worldActor.subActors.end() && containsInsensitive(subActor->name, query);
}

} // namespace

void TrackTreeModel::setSearch(std::string_view query) { mSearch = query; }

void TrackTreeModel::setCamerasExpanded(bool expanded) { mCamerasExpanded = expanded; }

void TrackTreeModel::setCameraExpanded(std::string_view cameraId, bool expanded) {
    if (expanded) mExpandedCameras.emplace(cameraId);
    else mExpandedCameras.erase(std::string{cameraId});
}

void TrackTreeModel::toggleCameraExpanded(std::string_view cameraId) {
    setCameraExpanded(cameraId, !isCameraExpanded(cameraId));
}

bool TrackTreeModel::isCameraExpanded(std::string_view cameraId) const {
    return mExpandedCameras.find(std::string{cameraId}) != mExpandedCameras.end();
}

void TrackTreeModel::rebuild(const EditorStateExt& state) {
    mRows.clear();
    mRows.reserve(state.cameras.size() * 3 + 2);

    float const cameraHeight = metrics::cameraRow();
    float const subHeight    = metrics::subRow();

    if (mSearch.empty()) {
        mRows.push_back({TrackRowKind::World, "world", state.worldActor.name, {}, -1, cameraHeight, 0, false, true});
    }

    if (mCamerasExpanded) {
        for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
            const auto& camera = state.cameras[index];
            if (!cameraMatchesSearch(camera, state.worldActor, mSearch)) continue;
            bool const expanded = isCameraExpanded(camera.id);
            mRows.push_back(
                {TrackRowKind::Camera,
                 "camera:" + camera.id,
                 camera.name,
                 camera.id,
                 index,
                 cameraHeight,
                 0,
                 camera.locked,
                 camera.enabled,
                 true,
                 expanded}
            );
            if (!expanded) continue;
            mRows.push_back(
                {TrackRowKind::CameraTransform,
                 "camera:" + camera.id + ":transform",
                 {},
                 camera.id,
                 index,
                 subHeight,
                 1,
                 camera.locked,
                 camera.enabled}
            );
            mRows.push_back(
                {TrackRowKind::CameraFov,
                 "camera:" + camera.id + ":fov",
                 {},
                 camera.id,
                 index,
                 subHeight,
                 1,
                 camera.locked,
                 camera.enabled}
            );
        }
    }

    if (mSearch.empty()) {
        mRows.push_back({TrackRowKind::MarkerLane, "markers", {}, {}, -1, subHeight, 0, false, true});
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::editor::ui
