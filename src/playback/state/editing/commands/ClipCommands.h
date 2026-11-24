#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"
#include "playback/state/editing/models/SelectionModel.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class AddClip final : public model::IEditCommand {
public:
    AddClip(std::string cameraId, int inTick, int outTick, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    int                                  mInTick;
    int                                  mOutTick;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class SplitClip final : public model::IEditCommand {
public:
    SplitClip(std::string clipId, int splitTick, std::string newClipName = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    int                                  mSplitTick;
    std::string                          mNewClipName;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveAndTrimClip final : public model::IEditCommand {
public:
    MoveAndTrimClip(std::string clipId, int newInTick, int newOutTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    int                                  mNewInTick;
    int                                  mNewOutTick;
    std::optional<model::EditorStateExt> mBefore;
};

class RenameClip final : public model::IEditCommand {
public:
    RenameClip(std::string clipId, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class SetClipLocked final : public model::IEditCommand {
public:
    SetClipLocked(std::string clipId, bool locked);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    bool                                 mLocked;
    std::optional<model::EditorStateExt> mBefore;
};

class SetClipExportEnabled final : public model::IEditCommand {
public:
    SetClipExportEnabled(std::string clipId, bool exportEnabled);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    bool                                 mExportEnabled;
    std::optional<model::EditorStateExt> mBefore;
};

class SetAllClipsExportEnabled final : public model::IEditCommand {
public:
    explicit SetAllClipsExportEnabled(bool exportEnabled, std::string cameraId = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mExportEnabled;
    std::string                          mCameraId;
    std::optional<model::EditorStateExt> mBefore;
};

class ResetClipsToFullRange final : public model::IEditCommand {
public:
    explicit ResetClipsToFullRange(std::string cameraId = {}, std::string defaultClipName = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mCameraId;
    std::string                          mDefaultClipName;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteClip final : public model::IEditCommand {
public:
    explicit DeleteClip(std::string clipId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mClipId;
    std::optional<model::EditorStateExt> mBefore;
};

class HealClipCuts final : public model::IEditCommand {
public:
    explicit HealClipCuts(
        std::vector<model::SelectedClipCut> cuts,
        std::vector<model::KeyframeRef>     keyframesToDelete = {}
    );
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<model::SelectedClipCut>  mCuts;
    std::vector<model::KeyframeRef>      mKeyframesToDelete;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mDeletedKeyframes{false};
    bool                                 mHealedCuts{false};
};

class AddScene final : public model::IEditCommand {
public:
    explicit AddScene(std::string name = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class DuplicateScene final : public model::IEditCommand {
public:
    DuplicateScene(std::string sceneId, std::string copyName = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSceneId;
    std::string                          mCopyName;
    std::optional<model::EditorStateExt> mBefore;
};

class RenameScene final : public model::IEditCommand {
public:
    RenameScene(std::string sceneId, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSceneId;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveScene final : public model::IEditCommand {
public:
    MoveScene(std::string sceneId, int targetIndex);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSceneId;
    int                                  mTargetIndex;
    std::optional<model::EditorStateExt> mBefore;
};

class DeleteScene final : public model::IEditCommand {
public:
    explicit DeleteScene(std::string sceneId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSceneId;
    std::optional<model::EditorStateExt> mBefore;
};

class SwitchScene final : public model::IEditCommand {
public:
    explicit SwitchScene(std::string sceneId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mSceneId;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSceneExportRange final : public model::IEditCommand {
public:
    SetSceneExportRange(int inTick, int outTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mInTick;
    int                                  mOutTick;
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
