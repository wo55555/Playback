#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/models/IEditCommand.h"

#include <optional>
#include <string>

namespace playback::editor::editing::command {

class SplitWorldActorAtPlayhead final : public model::IEditCommand {
public:
    explicit SplitWorldActorAtPlayhead(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class TrimWorldActorSegment final : public model::IEditCommand {
public:
    TrimWorldActorSegment(std::string id, int start, int end);
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

class SetWorldActorSegmentSpeed final : public model::IEditCommand {
public:
    SetWorldActorSegmentSpeed(std::string id, float speed);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    float                                mSpeed;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

class RippleDeleteWorldActorSeg final : public model::IEditCommand {
public:
    explicit RippleDeleteWorldActorSeg(std::string id);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mChanged; }
    [[nodiscard]] std::string label() const override;

private:
    std::string                          mId;
    std::optional<model::EditorStateExt> mBefore;
    bool                                 mChanged{};
};

} // namespace playback::editor::editing::command
