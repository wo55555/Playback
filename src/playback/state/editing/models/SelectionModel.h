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

struct SelectedClipCut {
    std::string cameraId;
    int         startTick{};
    int endTick{}; // startTick == endTick: split seam between two adjacent clips; startTick < endTick: uncovered cut
                   // gap

    [[nodiscard]] bool isSeam() const { return startTick == endTick; }
    [[nodiscard]] bool isGap() const { return startTick < endTick; }
    bool               operator==(SelectedClipCut const&) const = default;
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

struct SelectedTimeOfDayTrack {};

struct SelectedTimeOfDayKeyframe {
    int tick{};
};

struct SelectedTimeOfDayKeyframes {
    std::vector<int> ticks;
};

struct SelectedWeatherTrack {};

struct SelectedWeatherKeyframe {
    int tick{};
};

struct SelectedWeatherKeyframes {
    std::vector<int> ticks;
};

struct SelectedAudioTrack {
    std::string trackId;
};

struct SelectedAudioClip {
    std::string trackId;
    std::string clipId;
};

using Selection = std::variant<
    SelectedKeyframe,
    SelectedClip,
    SelectedClipCut,
    SelectedMarker,
    SelectedTransition,
    SelectedCamera,
    SelectedSpeedTrack,
    SelectedSpeedKeyframe,
    SelectedFreezeTrack,
    SelectedFreezeKeyframe,
    SelectedTimelapseTrack,
    SelectedTimelapseKeyframe,
    SelectedTimeOfDayTrack,
    SelectedTimeOfDayKeyframe,
    SelectedTimeOfDayKeyframes,
    SelectedWeatherTrack,
    SelectedWeatherKeyframe,
    SelectedWeatherKeyframes,
    SelectedAudioTrack,
    SelectedAudioClip>;

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
    void                                           selectTimeOfDayTrack();
    void                                           selectTimeOfDayKeyframe(int tick);
    void                                           selectTimeOfDayKeyframes(std::vector<int> ticks);
    [[nodiscard]] bool                             isTimeOfDayTrackSelected() const;
    [[nodiscard]] std::optional<int>               selectedTimeOfDayKeyframe() const;
    [[nodiscard]] std::vector<int>                 selectedTimeOfDayKeyframes() const;
    [[nodiscard]] bool                             isTimeOfDayKeyframeSelected(int tick) const;
    void                                           selectWeatherTrack();
    void                                           selectWeatherKeyframe(int tick);
    void                                           selectWeatherKeyframes(std::vector<int> ticks);
    [[nodiscard]] bool                             isWeatherTrackSelected() const;
    [[nodiscard]] std::optional<int>               selectedWeatherKeyframe() const;
    [[nodiscard]] std::vector<int>                 selectedWeatherKeyframes() const;
    [[nodiscard]] bool                             isWeatherKeyframeSelected(int tick) const;
    void                                           selectAudioTrack(std::string trackId);
    void                                           selectAudioClip(std::string trackId, std::string clipId);
    [[nodiscard]] std::optional<std::string>       selectedAudioTrackId() const;
    [[nodiscard]] std::optional<SelectedAudioClip> selectedAudioClip() const;
    [[nodiscard]] bool                     hasSelection() const;
    [[nodiscard]] const Selection*         getSelection() const;
    [[nodiscard]] std::vector<std::string> selectedIds() const;

    // Drops the selection when the object it points at is gone, e.g. after reloading a project.
    void pruneInvalid(EditorStateExt const& state);

    // Multi-keyframe & multi-cut selection. The primary selection stays a SelectedKeyframe or
    // SelectedClipCut (preferring the most recently picked keyframe if any) and the sets always
    // contain it; selecting anything else empties the sets.
    void                                               setKeyframes(std::vector<SelectedKeyframe> keys);
    void                                               setClipCuts(std::vector<SelectedClipCut> cuts);
    void setBoxSelection(std::vector<SelectedKeyframe> keys, std::vector<SelectedClipCut> cuts);
    void                                               toggleKeyframe(SelectedKeyframe key);
    [[nodiscard]] bool                                 isKeyframeSelected(std::string const& trackId, int tick) const;
    [[nodiscard]] bool isClipCutSelected(std::string const& cameraId, int startTick, int endTick) const;
    [[nodiscard]] std::vector<SelectedKeyframe> const& keyframes() const { return mKeyframes; }
    [[nodiscard]] std::size_t                          keyframeCount() const { return mKeyframes.size(); }
    [[nodiscard]] std::vector<SelectedClipCut> const&  clipCuts() const { return mClipCuts; }
    [[nodiscard]] std::size_t                          clipCutCount() const { return mClipCuts.size(); }

    template <typename T>
    [[nodiscard]] const T* getAs() const {
        if (!mSelection) return nullptr;
        return std::get_if<T>(&(*mSelection));
    }

private:
    std::optional<Selection>      mSelection;
    std::vector<SelectedKeyframe> mKeyframes;
    std::vector<SelectedClipCut>  mClipCuts;
    std::string                   mLastSceneId;
};

} // namespace playback::state::editing::model
