#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::editor::ui {

enum class TrackRowKind { Camera, Marker };

struct TrackTreeRow {
    TrackRowKind kind;
    std::string  id;
    std::string  name;
    int          cameraIndex{-1};
    float        height{};
    bool         locked{};
    bool         enabled{true};
};

class TrackTreeModel {
public:
    static constexpr float kCameraRowHeight = 36.0f;
    static constexpr float kMarkerRowHeight = 32.0f;

    void                                           setSearch(std::string_view query);
    void                                           setCamerasExpanded(bool expanded);
    void                                           rebuild(state::editing::model::EditorStateExt const& state);
    [[nodiscard]] std::vector<TrackTreeRow> const& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
    bool                      mCamerasExpanded{true};
};

} // namespace playback::editor::ui
