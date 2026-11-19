#pragma once

#include "CameraKeyframe.h"
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
    std::string clipId;
};

struct SelectedMarker {
    std::string markerId;
};

struct SelectedTransition {
    std::string transitionId;
};

struct SelectedCamera {
    std::string cameraId;
};

using Selection = std::variant<SelectedKeyframe, SelectedClip, SelectedMarker, SelectedTransition, SelectedCamera>;

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
