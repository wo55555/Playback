#include "AsyncReplaySaver.h"

#include "playback/functions/action/Action.h"

#include "mc/deps/core/utility/BinaryStream.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace playback::functions {

void ReplayWriter::writeHeader() {
    // write file metadata
    mStream.writeType(MAGIC_NUMBER);
    mStream.writeType(FILE_VERSION);

    // write registry actions
    mActionNameToId.clear();
    auto&                         actions = functions::ActionRegistry::getInstance().getActions();
    std::vector<std::string_view> names;
    names.reserve(actions.size());
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        mActionNameToId[actions[i]->name] = i;
        names.push_back(actions[i]->name);
    }
    mStream.writeType(static_cast<int32_t>(names.size()));
    for (auto name : names) {
        mStream.writeType(name);
    }

    mState = STATE_EMPTY;
}

void ReplayWriter::startSnapshot() {
    if (mState != STATE_EMPTY) {
        throw std::runtime_error("Can only start snapshot in STATE_SNAPSHOT");
    }
    mState = STATE_WRITING_SNAPSHOT;

    mSnapshotSizePos = static_cast<int32_t>(mBuffer.size());
    mStream.writeType(static_cast<int32_t>(0xDEADBEEF));
}

void ReplayWriter::endSnapshot() {
    if (mState != STATE_WRITING_SNAPSHOT) {
        throw std::runtime_error("Can only end snapshot in STATE_SNAPSHOT");
    }
    mState = STATE_WRITING_DATA;

    if (mSnapshotSizePos < 0) {
        throw std::runtime_error(std::format("Snapshot size pos wasn't set ({})", this->mSnapshotSizePos));
    }

    int32_t snapshotSize = static_cast<int32_t>(mBuffer.size()) - mSnapshotSizePos - 4;
    *reinterpret_cast<int32_t*>(mBuffer.data() + mSnapshotSizePos) = snapshotSize;
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

    mStream.writeType(actionId);
    mStream.writeType(static_cast<int32_t>(0));

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

    mStream.writeType(actionId);
    mActionSizePos = static_cast<int32_t>(mBuffer.size());
    mStream.writeType(static_cast<int32_t>(0));
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

    int32_t dataSize = static_cast<int32_t>(mBuffer.size()) - mActionSizePos - 4;
    *reinterpret_cast<int32_t*>(mBuffer.data() + mActionSizePos) = dataSize;

    mActionSizePos = -1;
}

} // namespace playback::functions
