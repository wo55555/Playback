#include "TrackTreeModel.h"

#include "EditorStateExt.h"

#include <algorithm>
#include <cctype>

namespace playback::state::editing::model {

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

void TrackTreeModel::rebuild(const EditorStateExt& state) {
    mRows.clear();
    mRows.reserve(state.sequence.empty() ? state.cameras.size() : state.cameras.size() + 1);
    if (!state.sequence.empty()) {
        mRows.push_back({TrackRowKind::Sequence, "sequence", "Sequence", -1, kSequenceRowHeight, false, true});
    }

    if (mCamerasExpanded) {
        for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
            const auto& camera = state.cameras[index];
            if (!cameraMatchesSearch(camera, state.worldActor, mSearch)) continue;
            mRows.push_back(
                {TrackRowKind::Camera,
                 "camera:" + camera.id,
                 camera.name,
                 index,
                 kCameraRowHeight,
                 camera.locked,
                 camera.enabled}
            );
        }
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::state::editing::model
