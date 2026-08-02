#include "TrackTreeModel.h"

#include "EditorStateExt.h"

#include <algorithm>
#include <cctype>

namespace playback::editor::editing::model {

namespace {

bool containsInsensitive(std::string_view value, std::string_view query) {
    if (query.empty()) return true;
    return std::search(value.begin(), value.end(), query.begin(), query.end(), [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
    }) != value.end();
}

bool cameraMatchesSearch(const CameraEntity& camera, const WorldActor& worldActor, std::string_view query) {
    if (containsInsensitive(camera.name, query)) return true;
    auto subActor = std::find_if(worldActor.subActors.begin(), worldActor.subActors.end(), [&camera](const SubActor& actor) {
        return actor.id == camera.bindingEntityUuid;
    });
    return subActor != worldActor.subActors.end() && containsInsensitive(subActor->name, query);
}

}

void TrackTreeModel::setSearch(std::string_view query) {
    mSearch = query;
}

void TrackTreeModel::setCamerasExpanded(bool expanded) {
    mCamerasExpanded = expanded;
}

void TrackTreeModel::setMarkerExpanded(bool expanded) {
    mMarkerExpanded = expanded;
}

void TrackTreeModel::rebuild(const EditorStateExt& state) {
    mRows.clear();
    mRows.reserve(2 + state.cameras.size() + (mMarkerExpanded ? 1 : 0));
    mRows.push_back({TrackRowKind::Sequence, "sequence", "Sequence", -1, kSequenceRowHeight, false, false, true});
    mRows.push_back({TrackRowKind::WorldActor, "worldActor", "World Actor", -1, kWorldActorRowHeight, false, false, true});

    if (mCamerasExpanded) {
        for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
            const auto& camera = state.cameras[index];
            if (!cameraMatchesSearch(camera, state.worldActor, mSearch)) continue;
            mRows.push_back({TrackRowKind::Camera, "camera:" + camera.id, camera.name, index, kCameraRowHeight, camera.active, camera.locked, true});
        }
    }

    if (mMarkerExpanded) {
        mRows.push_back({TrackRowKind::Marker, "marker", "Markers", -1, kMarkerRowHeight, false, false, true});
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const {
    return mRows;
}

} // namespace playback::editor::editing::model
