#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::state::editing::command {

class AddAudioTrack final : public model::IEditCommand {
public:
    explicit AddAudioTrack(std::string name = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveAudioTrack final : public model::IEditCommand {
public:
    explicit RemoveAudioTrack(std::string trackId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::optional<model::EditorStateExt> mBefore;
};

class RenameAudioTrack final : public model::IEditCommand {
public:
    RenameAudioTrack(std::string trackId, std::string name);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class SetAudioTrackState final : public model::IEditCommand {
public:
    enum class Property { Enabled, Muted, Locked, Volume };

    SetAudioTrackState(std::string trackId, Property property, bool value);
    SetAudioTrackState(std::string trackId, float volume);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    Property                             mProperty;
    bool                                 mBoolValue{false};
    float                                mVolumeValue{1.0f};
    std::optional<model::EditorStateExt> mBefore;
};

class AddAudioClip final : public model::IEditCommand {
public:
    AddAudioClip(std::string trackId, std::string filePath, int startTick, std::string name = {});
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::string                          mFilePath;
    int                                  mStartTick{0};
    std::string                          mName;
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveAudioClip final : public model::IEditCommand {
public:
    RemoveAudioClip(std::string trackId, std::string clipId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::string                          mClipId;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveAudioClip final : public model::IEditCommand {
public:
    MoveAudioClip(std::string trackId, std::string clipId, int startTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::string                          mClipId;
    int                                  mStartTick{0};
    std::optional<model::EditorStateExt> mBefore;
};

class SetAudioClipProps final : public model::IEditCommand {
public:
    SetAudioClipProps(
        std::string trackId,
        std::string clipId,
        std::string filePath,
        double      trimStartSeconds,
        double      durationSeconds,
        float       volume,
        bool        muted
    );
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mTrackId;
    std::string                          mClipId;
    std::string                          mFilePath;
    double                               mTrimStartSeconds{0.0};
    double                               mDurationSeconds{0.0};
    float                                mVolume{1.0f};
    bool                                 mMuted{false};
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
