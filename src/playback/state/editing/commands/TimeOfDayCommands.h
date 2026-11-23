#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class AddTimeOfDayTrack final : public model::IEditCommand {
public:
    AddTimeOfDayTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveTimeOfDayTrack final : public model::IEditCommand {
public:
    RemoveTimeOfDayTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayTrackEnabled final : public model::IEditCommand {
public:
    explicit SetTimeOfDayTrackEnabled(bool enabled);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mEnabled;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayTrackLocked final : public model::IEditCommand {
public:
    explicit SetTimeOfDayTrackLocked(bool locked);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mLocked;
    std::optional<model::EditorStateExt> mBefore;
};

class AddTimeOfDayKeyframe final : public model::IEditCommand {
public:
    AddTimeOfDayKeyframe(
        int                            tick,
        int                            timeOfDay         = 6000,
        model::CameraInterpolationType interpolationType = model::CameraInterpolationType::Linear
    );
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    int                                  mTimeOfDay;
    model::CameraInterpolationType       mInterpolationType;
    bool                                 mReplaced{false};
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveTimeOfDayKeyframe final : public model::IEditCommand {
public:
    explicit RemoveTimeOfDayKeyframe(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveTimeOfDayKeyframes final : public model::IEditCommand {
public:
    explicit RemoveTimeOfDayKeyframes(std::vector<int> ticks);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveTimeOfDayKeyframe final : public model::IEditCommand {
public:
    MoveTimeOfDayKeyframe(int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveTimeOfDayKeyframes final : public model::IEditCommand {
public:
    MoveTimeOfDayKeyframes(std::vector<int> ticks, int tickDelta);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    int                                  mTickDelta;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayKeyframeValue final : public model::IEditCommand {
public:
    SetTimeOfDayKeyframeValue(int tick, int timeOfDay);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    int                                  mTimeOfDay;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayKeyframeInterpolation final : public model::IEditCommand {
public:
    SetTimeOfDayKeyframeInterpolation(int tick, model::CameraInterpolationType type);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::CameraInterpolationType       mType;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayKeyframeBezier final : public model::IEditCommand {
public:
    SetTimeOfDayKeyframeBezier(int tick, model::Vec2 ctrl1, model::Vec2 ctrl2);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::Vec2                          mCtrl1;
    model::Vec2                          mCtrl2;
    std::optional<model::EditorStateExt> mBefore;
};

class SetTimeOfDayKeyframesInterpolation final : public model::IEditCommand {
public:
    SetTimeOfDayKeyframesInterpolation(std::vector<int> ticks, model::CameraInterpolationType type);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    model::CameraInterpolationType       mType;
    std::optional<model::EditorStateExt> mBefore;
};

class ClearTimeOfDayKeyframes final : public model::IEditCommand {
public:
    ClearTimeOfDayKeyframes() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class SetSceneDaytimeOverride final : public model::IEditCommand {
public:
    SetSceneDaytimeOverride(bool enabled, int ticks);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mEnabled;
    int                                  mTicks;
    std::optional<model::EditorStateExt> mBefore;
};

class SetSceneWeatherOverride final : public model::IEditCommand {
public:
    explicit SetSceneWeatherOverride(model::WeatherOverride weather);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    model::WeatherOverride               mWeather;
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
