#include "AsyncReplaySaver.h"

#include "playback/functions/action/Action.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace playback::functions {

void ReplayWriter::writeHeader() {
    // write file metadata
    mStream.writeVarInt(MAGIC_NUMBER, nullptr, nullptr);

    // write registry actions
    auto&                         actions = functions::ActionRegistry::getInstance().getActions();
    std::vector<std::string_view> names;

    mActionNameToId.clear();
    names.reserve(actions.size());
    for (int32_t i = 0; i < static_cast<int32_t>(actions.size()); ++i) {
        mActionNameToId[actions[i]->name] = i;
        names.push_back(actions[i]->name);
    }
    mStream.writeVarInt(static_cast<int32_t>(names.size()), nullptr, nullptr);
    for (auto name : names) {
        mStream.writeString(name, nullptr, nullptr);
    }

    mState = STATE_EMPTY;
}

void ReplayWriter::startSnapshot() {
    if (mState != STATE_EMPTY) {
        throw std::runtime_error("Can only start snapshot in STATE_SNAPSHOT");
    }
    mState = STATE_WRITING_SNAPSHOT;

    mSnapshotSizePos = static_cast<int32_t>(mStream.getWritePointer());
    mStream.writeVarInt(static_cast<int32_t>(0xDEADBEEF), nullptr, nullptr);
}

void ReplayWriter::endSnapshot() {
    if (mState != STATE_WRITING_SNAPSHOT) {
        throw std::runtime_error("Can only end snapshot in STATE_SNAPSHOT");
    }
    mState = STATE_WRITING_DATA;

    if (mSnapshotSizePos < 0) {
        throw std::runtime_error(std::format("Snapshot size pos wasn't set ({})", this->mSnapshotSizePos));
    }

    int32_t snapshotSize = static_cast<int32_t>(mStream.getWritePointer() - mSnapshotSizePos) - 4;
    mStream.writeAt(mSnapshotSizePos, snapshotSize);
}

void ReplayWriter::startAndFinishAction(Action& action) {
    if (mWritingAction == nullptr) {
        throw std::runtime_error(std::format("startAndFinishAction() called while still writing {}", action.name));
    }

    auto it = mActionNameToId.find(action.name);
    if (it == mActionNameToId.end()) {
        throw std::runtime_error(std::format("Unknown action: {}", action.name));
    }
    int32_t actionId = it->second;

    mStream.writeVarInt(actionId, nullptr, nullptr);
    mStream.writeVarInt(static_cast<int32_t>(0), nullptr, nullptr);

    mActionSizePos = -1;
}

void ReplayWriter::startAction(Action& action) {
    if (mWritingAction != nullptr) {
        throw std::runtime_error(std::format("startAction() called while still writing {}", action.name));
    }
    mWritingAction = &action;

    auto it = mActionNameToId.find(action.name);
    if (it == mActionNameToId.end()) {
        throw std::runtime_error(std::format("Unknown action: {}", action.name));
    }
    int32_t actionId = it->second;

    mStream.writeVarInt(actionId, nullptr, nullptr);
    mActionSizePos = static_cast<int32_t>(mStream.getWritePointer());
    mStream.writeVarInt(static_cast<int32_t>(0), nullptr, nullptr);
}

void ReplayWriter::finishAction(Action& action) {
    if (mWritingAction == nullptr) {
        throw std::runtime_error("finishAction() called before startAction()");
    }
    if (mWritingAction != &action) {
        throw std::runtime_error(
            std::format(
                "finishAction() called with wrong action, expected {} got {}",
                mWritingAction->name,
                action.name
            )
        );
    }
    mWritingAction = nullptr;

    if (mActionSizePos < 0) {
        throw std::runtime_error(std::format("Action size pos wasn't set ({})", mActionSizePos));
    }

    int32_t actionSize = static_cast<int32_t>(mStream.getWritePointer() - mActionSizePos) - 4;
    mStream.writeAt(mActionSizePos, actionSize);

    mActionSizePos = -1;
}

} // namespace playback::functions
