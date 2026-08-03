#include "SequenceCommands.h"

#include "playback/editor/editing/SequenceOps.h"

#include <algorithm>
#include <utility>

namespace playback::editor::editing::command {
namespace {

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

SplitSequenceAtPlayhead::SplitSequenceAtPlayhead(int tick) : mTick(tick) {}

void SplitSequenceAtPlayhead::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = !SequenceOps::splitAt(state.sequence, mTick).empty();
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        SplitSequenceAtPlayhead::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string SplitSequenceAtPlayhead::label() const { return "Split Sequence"; }

TrimSequenceSegment::TrimSequenceSegment(std::string id, int start, int end)
: mId(std::move(id)),
  mStart(start),
  mEnd(end) {}

void TrimSequenceSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    mChanged    = SequenceOps::trimSegment(state.sequence, mId, mStart, mEnd);
    mBefore     = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        TrimSequenceSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string TrimSequenceSegment::label() const { return "Trim Sequence"; }

DeleteSequenceSegment::DeleteSequenceSegment(std::string id) : mId(std::move(id)) {}

void DeleteSequenceSegment::execute(model::EditorStateExt& state) {
    auto before = state;
    auto it     = std::find_if(state.sequence.begin(), state.sequence.end(), [&](auto const& segment) {
        return segment.id == mId;
    });
    mChanged    = it != state.sequence.end()
            && SequenceOps::deleteSegment(
                   state.sequence,
                   static_cast<size_t>(std::distance(state.sequence.begin(), it)),
                   state.totalTicks
            );
    mBefore = mChanged ? std::optional<model::EditorStateExt>(std::move(before)) : std::nullopt;
}

void        DeleteSequenceSegment::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteSequenceSegment::label() const { return "Delete Sequence Segment"; }

BindSequenceToCamera::BindSequenceToCamera(std::string id, std::string cameraId)
: mId(std::move(id)),
  mCameraId(std::move(cameraId)) {}

void BindSequenceToCamera::execute(model::EditorStateExt& state) {
    mChanged = false;
    auto segment =
        std::find_if(state.sequence.begin(), state.sequence.end(), [&](auto const& value) { return value.id == mId; });
    bool const cameraExists =
        mCameraId.empty() || std::any_of(state.cameras.begin(), state.cameras.end(), [&](auto const& camera) {
            return camera.id == mCameraId;
        });
    if (segment == state.sequence.end() || segment->locked || segment->cameraId == mCameraId || !cameraExists) {
        mBefore.reset();
        return;
    }

    mBefore = state;
    SequenceOps::bindCamera(*segment, mCameraId);
    mChanged = true;
}

void        BindSequenceToCamera::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string BindSequenceToCamera::label() const { return "Bind Sequence Camera"; }

} // namespace playback::editor::editing::command
