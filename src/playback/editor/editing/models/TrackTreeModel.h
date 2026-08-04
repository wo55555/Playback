#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playback::editor::editing::model {

struct EditorStateExt;

enum class TrackRowKind {
    Sequence,
    Camera,
    Marker
};

struct TrackTreeRow {
    TrackRowKind kind;
    std::string id;
    std::string name;
    int cameraIndex{-1};
    float height{};
    bool active{};
    bool locked{};
    bool visible{true};
};

class TrackTreeModel {
public:
    static constexpr float kSequenceRowHeight = 52.0f;
    static constexpr float kCameraRowHeight = 36.0f;
    static constexpr float kMarkerRowHeight = 32.0f;

    void setSearch(std::string_view query);
    void setCamerasExpanded(bool expanded);
    void rebuild(const EditorStateExt& state);
    [[nodiscard]] const std::vector<TrackTreeRow>& rows() const;

private:
    std::string mSearch;
    std::vector<TrackTreeRow> mRows;
    bool mCamerasExpanded{true};
};

} // namespace playback::editor::editing::model
