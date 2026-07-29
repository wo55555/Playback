#pragma once

#include "playback/refactor/editor/models/EditorStateExt.h"
#include "playback/refactor/editor/models/Track.h"

#include <memory>
#include <string>
#include <vector>

namespace playback::refactor::video_editing {

using namespace playback::refactor::editor;

// ===== IEditCommand (video-editing version) =====

class IEditCommand {
public:
    virtual ~IEditCommand() = default;
    virtual void execute(EditorStateExt& s) = 0;
    virtual void undo(EditorStateExt& s) = 0;
    [[nodiscard]] virtual std::string label() const = 0;
};

// ===== AddClipCommand =====

class AddClipCommand : public IEditCommand {
public:
    AddClipCommand(const std::string& trackId, const Clip& clip);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override { return "Add Clip"; }

private:
    std::string mTrackId;
    Clip        mClip;
    std::string mAddedClipId;
};

// ===== RemoveClipCommand =====

class RemoveClipCommand : public IEditCommand {
public:
    RemoveClipCommand(const std::string& trackId, const std::string& clipId);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override { return "Remove Clip"; }

private:
    std::string mTrackId;
    std::string mClipId;
    Clip        mSavedClip;
    size_t      mSavedIndex{};
};

// ===== SplitClipCommand =====

class SplitClipCommand : public IEditCommand {
public:
    SplitClipCommand(const std::string& trackId, const std::string& clipId, int atTick);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override;

private:
    std::string mTrackId;
    std::string mClipId;
    int         mAtTick{};
    Clip        mRightClip;  // The split-off right half
    std::string mRightClipId;
    int         mOldOutTick{};
};

// ===== TrimClipCommand =====

class TrimClipCommand : public IEditCommand {
public:
    TrimClipCommand(const std::string& trackId, const std::string& clipId,
                    int newInTick, int newOutTick);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override { return "Trim Clip"; }

private:
    std::string mTrackId;
    std::string mClipId;
    int         mOldInTick{};
    int         mOldOutTick{};
    int         mNewInTick{};
    int         mNewOutTick{};
};

// ===== MoveClipCommand =====

class MoveClipCommand : public IEditCommand {
public:
    MoveClipCommand(const std::string& trackId, const std::string& clipId, int newTrackTick);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override { return "Move Clip"; }

private:
    std::string mTrackId;
    std::string mClipId;
    int         mOldTrackTick{};
    int         mNewTrackTick{};
};

// ===== AddTransitionCommand =====

class AddTransitionCommand : public IEditCommand {
public:
    AddTransitionCommand(const std::string& fromClipId, const std::string& toClipId,
                         TransitionKind kind, int durationTicks);
    void execute(EditorStateExt& s) override;
    void undo(EditorStateExt& s) override;
    std::string label() const override { return "Add Transition"; }

private:
    std::string mFromClipId;
    std::string mToClipId;
    TransitionKind mKind;
    int         mDurationTicks{};
    std::string mAddedTransitionId;
};

} // namespace playback::refactor::video_editing