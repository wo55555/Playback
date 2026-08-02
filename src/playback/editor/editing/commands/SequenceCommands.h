#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {

class SplitSequenceAtPlayhead final : public model::IEditCommand {
public:
    explicit SplitSequenceAtPlayhead(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class TrimSequenceSegment final : public model::IEditCommand {
public:
    TrimSequenceSegment(std::string id, int start, int end);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    int                                  mStart;
    int                                  mEnd;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class DeleteSequenceSegment final : public model::IEditCommand {
public:
    explicit DeleteSequenceSegment(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class BindSequenceToCamera final : public model::IEditCommand {
public:
    BindSequenceToCamera(std::string id, std::string cameraId);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::string                          mCameraId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

} // namespace playback::editor::editing::command
