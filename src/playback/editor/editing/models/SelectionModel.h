#pragma once

#include "CameraKeyframe.h"
#include "SequenceSegment.h"
#include "SubActor.h"
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

struct SelectedSequence {};
struct SelectedSequenceSegment {
    std::string segmentId;
};
struct SelectedWorldActor {};
struct SelectedWorldActorSegment {
    std::string segmentId;
};
struct SelectedSubActor {
    std::string subActorId;
};
struct SelectedCamera {
    std::string cameraId;
};

using Selection = std::variant<
    SelectedKeyframe,
    SelectedClip,
    SelectedMarker,
    SelectedTrack,
    SelectedTransition,
    SelectedSequence,
    SelectedSequenceSegment,
    SelectedWorldActor,
    SelectedWorldActorSegment,
    SelectedSubActor,
    SelectedCamera>;

class SelectionModel {
public:
    void                                   select(Selection sel);
    void                                   clear();
    [[nodiscard]] bool                     hasSelection() const;
    [[nodiscard]] const Selection*         getSelection() const;
    [[nodiscard]] std::vector<std::string> selectedIds() const;

    template <typename T>
    [[nodiscard]] const T* getAs() const {
        if (!mSelection) return nullptr;
        return std::get_if<T>(&(*mSelection));
    }

private:
    std::optional<Selection> mSelection;
};

} // namespace playback::editor::editing::model
