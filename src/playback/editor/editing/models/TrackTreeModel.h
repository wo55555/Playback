#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace playback::editor::editing::model {

struct EditorStateExt;

enum class TrackRowKind { Sequence, WorldActor, Camera, Marker };

struct TrackTreeRow {
    TrackRowKind kind;
    std::string  id;
    std::string  name;
    int          cameraIndex{-1};
    float        height{};
    bool         active{};
    bool         locked{};
    bool         visible{true};
};

class TrackTreeModel {
public:
    static constexpr float kSequenceRowHeight   = 48.0f;
    static constexpr float kWorldActorRowHeight = 48.0f;
    static constexpr float kCameraRowHeight     = 24.0f;
    static constexpr float kMarkerRowHeight     = 20.0f;

    void                                           setSearch(std::string_view query);
    void                                           setCamerasExpanded(bool expanded);
    void                                           setMarkerExpanded(bool expanded);
    void                                           rebuild(const EditorStateExt& state);
    [[nodiscard]] const std::vector<TrackTreeRow>& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
    bool                      mCamerasExpanded{true};
    bool                      mMarkerExpanded{true};
};

} // namespace playback::editor::editing::model
