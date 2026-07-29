#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/Track.h"

#include <mutex>
#include <string>
#include <vector>

namespace playback::refactor::video_editing {

using namespace playback::refactor::editor;

class TrackManager {
public:
    static TrackManager& getInstance();

    void setEditorState(const EditorStateExt& state);
    EditorStateExt snapshot() const;

    // Track operations
    std::string addTrack(TrackKind kind, const std::string& name);
    void removeTrack(const std::string& id);
    void reorderTrack(const std::string& id, int newIndex);

    // Clip operations
    std::string addClip(const std::string& trackId, const Clip& clip);
    void removeClip(const std::string& trackId, const std::string& clipId);
    void moveClip(const std::string& trackId, const std::string& clipId, int newTrackTick);
    void trimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick);
    void splitClip(const std::string& trackId, const std::string& clipId, int atTick);
    void rippleDelete(const std::string& trackId, const std::string& clipId);

    // Transition operations
    std::string addTransition(const std::string& fromClipId, const std::string& toClipId,
                              TransitionKind kind, int durationTicks);
    void removeTransition(const std::string& transitionId);

    // Query
    std::vector<Clip*> getActiveClipsAt(int timelineTick);
    const Transition* findTransitionBetween(const std::string& fromClipId,
                                            const std::string& toClipId) const;

private:
    TrackManager() = default;

    Track&       findTrack(const std::string& id);
    const Track& findTrack(const std::string& id) const;
    std::vector<Clip>::iterator       findClipIter(Track& track, const std::string& clipId);
    Clip&       findClip(const std::string& trackId, const std::string& clipId);
    const Clip& findClip(const std::string& trackId, const std::string& clipId) const;

    static void sortClipsByTick(Track& track);
    static Color4 pickColorFor(const std::string& replayFile);

    mutable std::mutex mMtx;
    EditorStateExt     mState;
};

} // namespace playback::refactor::video_editing