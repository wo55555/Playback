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

struct SelectedSpeedTrack {};

struct SelectedSpeedKeyframe {
    int tick{};
};

struct SelectedFreezeTrack {};

struct SelectedFreezeKeyframe {
    int tick{};
};

struct SelectedTimelapseTrack {};

struct SelectedTimelapseKeyframe {
    int tick{};
};

using Selection = std::variant<
    SelectedKeyframe,
    SelectedClip,
    SelectedMarker,
    SelectedTransition,
    SelectedCamera,
    SelectedSpeedTrack,
    SelectedSpeedKeyframe,
    SelectedFreezeTrack,
    SelectedFreezeKeyframe,
    SelectedTimelapseTrack,
    SelectedTimelapseKeyframe>;

struct EditorStateExt;

class SelectionModel {
public:
    void                                   select(Selection sel);
    void                                   clear();
    void                                   selectSpeedTrack();
    void                                   selectSpeedKeyframe(int tick);
    [[nodiscard]] bool                     isSpeedTrackSelected() const;
    [[nodiscard]] std::optional<int>       selectedSpeedKeyframe() const;
    void                                   selectFreezeTrack();
    void                                   selectFreezeKeyframe(int tick);
    [[nodiscard]] bool                     isFreezeTrackSelected() const;
    [[nodiscard]] std::optional<int>       selectedFreezeKeyframe() const;
    void                                   selectTimelapseTrack();
    void                                   selectTimelapseKeyframe(int tick);
    [[nodiscard]] bool                     isTimelapseTrackSelected() const;
    [[nodiscard]] std::optional<int>       selectedTimelapseKeyframe() const;
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
    std::string                   mLastSceneId;
};

} // namespace playback::state::editing::model
