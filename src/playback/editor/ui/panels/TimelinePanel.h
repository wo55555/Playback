#pragma once

namespace playback::editor::ui {

class TimelinePanel {
public:
    void draw();

    [[nodiscard]] float trackListWidthRatio() const { return mTrackListWidthRatio; }
    [[nodiscard]] float pixelsPerTick() const { return mPixelsPerTick; }
    [[nodiscard]] float horizontalScroll() const { return mScrollX; }
    void                setViewPreferences(float trackListWidthRatio, float pixelsPerTick, float horizontalScroll);

private:
    void submitSeek(int tick);

    float mPixelsPerTick{0.25f};
    float mScrollX{};
    float mTrackListWidthRatio{0.30f};
    int   mPendingSeekTick{-1};
};

} // namespace playback::editor::ui
