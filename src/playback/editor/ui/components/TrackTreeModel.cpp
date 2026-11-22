#include "TrackTreeModel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include <algorithm>
#include <cctype>
#include <ranges>

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

bool cameraMatchesSearch(const CameraEntity& camera, std::string_view query) {
    return containsInsensitive(camera.name, query);
}

} // namespace

void TrackTreeModel::setSearch(std::string_view query) { mSearch = query; }

void TrackTreeModel::rebuild(const EditorStateExt& state) {
    mRows.clear();
    mRows.reserve(state.cameras.size());

    float const rowHeight = metrics::cameraRow();

    for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
        const auto& camera = state.cameras[index];
        if (!cameraMatchesSearch(camera, mSearch)) continue;
        mRows.push_back(
            {"camera:" + camera.id, camera.name, camera.id, index, rowHeight, camera.locked, camera.enabled}
        );
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::editor::ui
