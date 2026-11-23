#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/state/editing/models/IEditCommand.h"

#include <optional>
#include <string>
#include <vector>

namespace playback::state::editing::command {

class AddWeatherTrack final : public model::IEditCommand {
public:
    AddWeatherTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveWeatherTrack final : public model::IEditCommand {
public:
    RemoveWeatherTrack() = default;
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::optional<model::EditorStateExt> mBefore;
};

class SetWeatherTrackEnabled final : public model::IEditCommand {
public:
    explicit SetWeatherTrackEnabled(bool enabled);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mEnabled;
    std::optional<model::EditorStateExt> mBefore;
};

class SetWeatherTrackLocked final : public model::IEditCommand {
public:
    explicit SetWeatherTrackLocked(bool locked);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    bool                                 mLocked;
    std::optional<model::EditorStateExt> mBefore;
};

class AddWeatherKeyframe final : public model::IEditCommand {
public:
    explicit AddWeatherKeyframe(int tick, model::WeatherOverride weather = model::WeatherOverride::Clear);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::WeatherOverride               mWeather;
    bool                                 mReplaced{false};
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveWeatherKeyframe final : public model::IEditCommand {
public:
    explicit RemoveWeatherKeyframe(int tick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    std::optional<model::EditorStateExt> mBefore;
};

class RemoveWeatherKeyframes final : public model::IEditCommand {
public:
    explicit RemoveWeatherKeyframes(std::vector<int> ticks);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveWeatherKeyframe final : public model::IEditCommand {
public:
    MoveWeatherKeyframe(int fromTick, int toTick);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mFromTick;
    int                                  mToTick;
    std::optional<model::EditorStateExt> mBefore;
};

class MoveWeatherKeyframes final : public model::IEditCommand {
public:
    MoveWeatherKeyframes(std::vector<int> ticks, int tickDelta);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    int                                  mTickDelta;
    std::optional<model::EditorStateExt> mBefore;
};

class SetWeatherKeyframeValue final : public model::IEditCommand {
public:
    SetWeatherKeyframeValue(int tick, model::WeatherOverride weather);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    int                                  mTick;
    model::WeatherOverride               mWeather;
    std::optional<model::EditorStateExt> mBefore;
};

class SetWeatherKeyframesValue final : public model::IEditCommand {
public:
    SetWeatherKeyframesValue(std::vector<int> ticks, model::WeatherOverride weather);
    void                      execute(model::EditorStateExt& state) override;
    void                      undo(model::EditorStateExt& state) override;
    [[nodiscard]] bool        didChange() const override { return mBefore.has_value(); }
    [[nodiscard]] std::string label() const override;

private:
    std::vector<int>                     mTicks;
    model::WeatherOverride               mWeather;
    std::optional<model::EditorStateExt> mBefore;
};

} // namespace playback::state::editing::command
