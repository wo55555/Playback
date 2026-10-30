#pragma once

#include "CameraKeyframe.h"
#include "SequenceSegment.h"
#include "SubActor.h"
#include "Track.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace playback::state::editing::model {

struct SelectedKeyframe {
    std::string trackId;
    int         tick{};
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

struct EditorStateExt;

class SelectionModel {
public:
    void                                   select(Selection sel);
    void                                   clear();
    [[nodiscard]] bool                     hasSelection() const;
    [[nodiscard]] const Selection*         getSelection() const;
    [[nodiscard]] std::vector<std::string> selectedIds() const;

    // Drops the selection when the object it points at is gone, e.g. after reloading a project.
    void pruneInvalid(EditorStateExt const& state);

    // Multi-keyframe selection. The primary selection stays a SelectedKeyframe (the most recently
    // picked one) and the set always contains it; selecting anything else empties the set.
    void                                               setKeyframes(std::vector<SelectedKeyframe> keys);
    void                                               toggleKeyframe(SelectedKeyframe key);
    [[nodiscard]] bool                                 isKeyframeSelected(std::string const& trackId, int tick) const;
    [[nodiscard]] std::vector<SelectedKeyframe> const& keyframes() const { return mKeyframes; }
    [[nodiscard]] std::size_t                          keyframeCount() const { return mKeyframes.size(); }

    template <typename T>
    [[nodiscard]] const T* getAs() const {
        if (!mSelection) return nullptr;
        return std::get_if<T>(&(*mSelection));
    }

private:
    std::optional<Selection>      mSelection;
    std::vector<SelectedKeyframe> mKeyframes;
};

} // namespace playback::state::editing::model
