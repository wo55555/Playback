#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/Track.h"

#include <mutex>
#include <string>
#include <vector>

namespace playback::state::editing {

class TrackManager {
public:
    static TrackManager& getInstance();

    void                  setEditorState(const model::EditorStateExt& state);
    model::EditorStateExt snapshot() const;

    // Track operations
    std::string addTrack(model::TrackKind kind, const std::string& name);
    void        removeTrack(const std::string& id);
    void        reorderTrack(const std::string& id, int newIndex);

    // Clip operations
    std::string addClip(const std::string& trackId, const model::Clip& clip);
    void        removeClip(const std::string& trackId, const std::string& clipId);
    void        moveClip(const std::string& trackId, const std::string& clipId, int newTrackTick);
    void        trimClip(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick);
    void        splitClip(const std::string& trackId, const std::string& clipId, int atTick);
    void        rippleDelete(const std::string& trackId, const std::string& clipId);

    // Transition operations
    std::string addTransition(
        const std::string&    fromClipId,
        const std::string&    toClipId,
        model::TransitionKind kind,
        int                   durationTicks
    );
    void removeTransition(const std::string& transitionId);

    // Query
    std::vector<model::Clip*> getActiveClipsAt(int timelineTick);
    const model::Transition*  findTransitionBetween(const std::string& fromClipId, const std::string& toClipId) const;

private:
    TrackManager() = default;

    model::Track&                      findTrack(const std::string& id);
    const model::Track&                findTrack(const std::string& id) const;
    std::vector<model::Clip>::iterator findClipIter(model::Track& track, const std::string& clipId);
    model::Clip&                       findClip(const std::string& trackId, const std::string& clipId);
    const model::Clip&                 findClip(const std::string& trackId, const std::string& clipId) const;

    static void          sortClipsByTick(model::Track& track);
    static model::Color4 pickColorFor(const std::string& replayFile);

    mutable std::mutex    mMtx;
    model::EditorStateExt mState;
};

} // namespace playback::state::editing
