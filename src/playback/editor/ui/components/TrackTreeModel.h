#pragma once

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace playback::state::editing::model {
struct EditorStateExt;
}

namespace playback::editor::ui {

enum class TrackRowKind { Camera };

struct TrackTreeRow {
    TrackRowKind kind;
    std::string  id;
    std::string  name;
    std::string  cameraId;
    int          cameraIndex{-1};
    float        height{};
    bool         locked{};
    bool         enabled{true};
};

class TrackTreeModel {
public:
    void                                           setSearch(std::string_view query);
    void                                           rebuild(state::editing::model::EditorStateExt const& state);
    [[nodiscard]] std::vector<TrackTreeRow> const& rows() const;

private:
    std::string               mSearch;
    std::vector<TrackTreeRow> mRows;
};

} // namespace playback::editor::ui
