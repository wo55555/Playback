#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <array>

namespace playback::editor::ui {

class DetailsPanel {
public:
    void draw(PanelContext const& ctx);

private:
    // Each returns true when it owns the current selection and has drawn it.
    bool drawWorldActor(PanelContext const& ctx);
    bool drawWorldActorSegment(PanelContext const& ctx);
    bool drawSubActor(PanelContext const& ctx);
    bool drawCamera(PanelContext const& ctx);
    bool drawKeyframe(PanelContext const& ctx);
    bool drawMarker(PanelContext const& ctx);
    void drawOverview(PanelContext const& ctx);

    std::array<char, 128> mSearch{};
};

} // namespace playback::editor::ui
