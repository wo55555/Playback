#include "MarkerCommands.h"

#include <algorithm>
#include <utility>

namespace playback::state::editing::command {
namespace {

model::Marker* findMarker(model::EditorStateExt& state, std::string const& id) {
    auto const it = std::ranges::find(state.markers, id, &model::Marker::id);
    return it == state.markers.end() ? nullptr : &*it;
}

bool tickTaken(model::EditorStateExt const& state, int tick, std::string const& except) {
    return std::ranges::any_of(state.markers, [&](auto const& marker) {
        return marker.tick == tick && marker.id != except;
    });
}

void sortMarkers(model::EditorStateExt& state) {
    std::ranges::stable_sort(state.markers, {}, &model::Marker::tick);
}

void restore(std::optional<model::EditorStateExt> const& before, model::EditorStateExt& state) {
    if (before) state = *before;
}

} // namespace

std::string nextMarkerId(model::EditorStateExt const& state) {
    for (int index = static_cast<int>(state.markers.size()) + 1;; ++index) {
        auto id = "marker_" + std::to_string(index);
        if (std::ranges::find(state.markers, id, &model::Marker::id) == state.markers.end()) return id;
    }
}

AddMarker::AddMarker(std::string id, int tick, std::string label)
: mId(std::move(id)),
  mTick(tick),
  mLabel(std::move(label)) {}

void AddMarker::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const tick = std::clamp(mTick, 0, state.totalTicks);
    if (tickTaken(state, tick, {})) return;
    auto id = mId.empty() ? nextMarkerId(state) : mId;
    if (findMarker(state, id)) return;

    mBefore = state;
    state.markers.push_back({std::move(id), mLabel, tick});
    sortMarkers(state);
}

void        AddMarker::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string AddMarker::label() const { return "Add Marker"; }

DeleteMarker::DeleteMarker(std::string id) : mId(std::move(id)) {}

void DeleteMarker::execute(model::EditorStateExt& state) {
    mBefore.reset();
    if (!findMarker(state, mId)) return;
    mBefore = state;
    std::erase_if(state.markers, [&](auto const& marker) { return marker.id == mId; });
}

void        DeleteMarker::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string DeleteMarker::label() const { return "Delete Marker"; }

MoveMarker::MoveMarker(std::string id, int tick) : mId(std::move(id)), mTick(tick) {}

void MoveMarker::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto const tick   = std::clamp(mTick, 0, state.totalTicks);
    auto*      marker = findMarker(state, mId);
    if (!marker || marker->tick == tick || tickTaken(state, tick, mId)) return;

    mBefore      = state;
    findMarker(state, mId)->tick = tick;
    sortMarkers(state);
}

void        MoveMarker::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string MoveMarker::label() const { return "Move Marker"; }

RenameMarker::RenameMarker(std::string id, std::string label) : mId(std::move(id)), mLabel(std::move(label)) {}

void RenameMarker::execute(model::EditorStateExt& state) {
    mBefore.reset();
    auto* marker = findMarker(state, mId);
    if (!marker || marker->label == mLabel) return;

    mBefore                       = state;
    findMarker(state, mId)->label = mLabel;
}

void        RenameMarker::undo(model::EditorStateExt& state) { restore(mBefore, state); }
std::string RenameMarker::label() const { return "Rename Marker"; }

} // namespace playback::state::editing::command
