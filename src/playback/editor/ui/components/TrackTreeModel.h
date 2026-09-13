#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::editor::ui {

enum class TrackRowKind { World, Camera, CameraTransform, CameraFov, MarkerLane };

struct TrackTreeRow {
    TrackRowKind kind;
    std::string  id;
    std::string  name;
    std::string  cameraId;
    int          cameraIndex{-1};
    float        height{};
    int          indent{};
    bool         locked{};
    bool         enabled{true};
    bool         expandable{};
    bool         expanded{};
};

class TrackTreeModel {
public:
    void                                           setSearch(std::string_view query);
    void                                           setCamerasExpanded(bool expanded);
    void                                           setCameraExpanded(std::string_view cameraId, bool expanded);
    void                                           toggleCameraExpanded(std::string_view cameraId);
    [[nodiscard]] bool                             isCameraExpanded(std::string_view cameraId) const;
    void                                           rebuild(state::editing::model::EditorStateExt const& state);
    [[nodiscard]] std::vector<TrackTreeRow> const& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
    std::set<std::string>     mExpandedCameras;
    bool                      mCamerasExpanded{true};
};

} // namespace playback::editor::ui
