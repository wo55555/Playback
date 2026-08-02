#pragma once

#include "CameraKeyframe.h"
#include "Track.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace playback::editor::editing::model {

// What can be selected in the editor
struct SelectedKeyframe {
    std::string trackId;
    std::string keyframeId;
};

struct SelectedClip {
    std::string trackId;
    std::string clipId;
};

struct SelectedMarker {
    std::string markerId;
};

struct SelectedTrack {
    std::string trackId;
};

struct SelectedTransition {
    std::string transitionId;
};

using Selection = std::variant<
    SelectedKeyframe,
    SelectedClip,
    SelectedMarker,
    SelectedTrack,
    SelectedTransition
>;

class SelectionModel {
public:
    void select(Selection sel);
    void clear();
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] const Selection* getSelection() const;
    [[nodiscard]] std::vector<std::string> selectedIds() const;

    // Event
    // using Callback = void(Selection);
    // Event<Callback> onSelectionChanged;

    template<typename T>
    [[nodiscard]] const T* getAs() const {
        if (!mSelection) return nullptr;
        return std::get_if<T>(&(*mSelection));
    }

private:
    std::optional<Selection> mSelection;
};

} // namespace playback::editor::editing::model