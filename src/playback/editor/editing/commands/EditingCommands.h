#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"
#include "playback/editor/editing/models/Track.h"

#include <memory>
#include <string>
#include <vector>

namespace playback::editor::editing::command {

// ===== IEditCommand (video-editing version) =====

class IEditCommand : public playback::editor::editing::model::IEditCommand {
public:
    virtual ~IEditCommand()                                              = default;
    void                      execute(model::EditorStateExt& s) override = 0;
    void                      undo(model::EditorStateExt& s) override    = 0;
    [[nodiscard]] std::string label() const override                     = 0;
};

// ===== AddClipCommand =====

class AddClipCommand : public IEditCommand {
public:
    AddClipCommand(const std::string& trackId, const model::Clip& clip);
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
    std::string label() const override { return "Add Clip"; }

private:
    std::string mTrackId;
    model::Clip mClip;
    std::string mAddedClipId;
};

// ===== RemoveClipCommand =====

class RemoveClipCommand : public IEditCommand {
public:
    RemoveClipCommand(const std::string& trackId, const std::string& clipId);
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
    std::string label() const override { return "Remove Clip"; }

private:
    std::string mTrackId;
    std::string mClipId;
    model::Clip mSavedClip;
    size_t      mSavedIndex{};
};

// ===== SplitClipCommand =====

class SplitClipCommand : public IEditCommand {
public:
    SplitClipCommand(const std::string& trackId, const std::string& clipId, int atTick);
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
    std::string label() const override;

private:
    std::string mTrackId;
    std::string mClipId;
    int         mAtTick{};
    model::Clip mRightClip; // The split-off right half
    std::string mRightClipId;
    int         mOldOutTick{};
};

// ===== TrimClipCommand =====

class TrimClipCommand : public IEditCommand {
public:
    TrimClipCommand(const std::string& trackId, const std::string& clipId, int newInTick, int newOutTick);
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
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
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
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
    AddTransitionCommand(
        const std::string&    fromClipId,
        const std::string&    toClipId,
        model::TransitionKind kind,
        int                   durationTicks
    );
    void        execute(model::EditorStateExt& s) override;
    void        undo(model::EditorStateExt& s) override;
    std::string label() const override { return "Add Transition"; }

private:
    std::string           mFromClipId;
    std::string           mToClipId;
    model::TransitionKind mKind;
    int                   mDurationTicks{};
    std::string           mAddedTransitionId;
};

} // namespace playback::editor::editing::command
