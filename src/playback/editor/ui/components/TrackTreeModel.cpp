#include "TrackTreeModel.h"

#include "playback/editor/ui/EditorTheme.h"
#include "playback/state/editing/models/EditorStateExt.h"

#include "ll/api/i18n/I18n.h"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace playback::editor::ui {

using namespace playback::state::editing::model;
using namespace ll::i18n_literals;

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
    mRows.reserve(state.cameras.size() + 3);

    float const rowHeight = metrics::cameraRow();

    for (int index = 0; index < static_cast<int>(state.cameras.size()); ++index) {
        const auto& camera = state.cameras[index];
        if (!cameraMatchesSearch(camera, mSearch)) continue;
        mRows.push_back(
            {TrackRowKind::Camera,
             "camera:" + camera.id,
             camera.name,
             camera.id,
             index,
             rowHeight,
             camera.locked,
             camera.enabled}
        );
    }

    if (state.speedTrack.isVisible()) {
        std::string speedName = "playback.refactorEditor.speed.trackName"_tr();
        if (containsInsensitive(speedName, mSearch)) {
            mRows.push_back(
                {TrackRowKind::Speed,
                 "speed",
                 std::move(speedName),
                 "__speed__",
                 -1,
                 rowHeight,
                 state.speedTrack.locked,
                 state.speedTrack.enabled}
            );
        }
    }

    if (state.freezeTrack.isVisible()) {
        std::string freezeName = "playback.refactorEditor.freeze.trackName"_tr();
        if (containsInsensitive(freezeName, mSearch)) {
            mRows.push_back(
                {TrackRowKind::Freeze,
                 "freeze",
                 std::move(freezeName),
                 "__freeze__",
                 -1,
                 rowHeight,
                 state.freezeTrack.locked,
                 state.freezeTrack.enabled}
            );
        }
    }

    if (state.timelapseTrack.isVisible()) {
        std::string timelapseName = "playback.refactorEditor.timelapse.trackName"_tr();
        if (containsInsensitive(timelapseName, mSearch)) {
            mRows.push_back(
                {TrackRowKind::Timelapse,
                 "timelapse",
                 std::move(timelapseName),
                 "__timelapse__",
                 -1,
                 rowHeight,
                 state.timelapseTrack.locked,
                 state.timelapseTrack.enabled}
            );
        }
    }
}

const std::vector<TrackTreeRow>& TrackTreeModel::rows() const { return mRows; }

} // namespace playback::editor::ui
