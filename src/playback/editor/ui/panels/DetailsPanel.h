#pragma once

#include "playback/editor/ui/PanelContext.h"

#include <array>
#include <optional>
#include <string>

namespace playback::editor::ui {

enum class InspectorPage { Selection, Export, Settings };

class DetailsPanel {
public:
    void draw(PanelContext const& ctx);

    [[nodiscard]] InspectorPage page() const { return mPage; }
    void                        setPage(InspectorPage page) { mPage = page; }
    void                        focusFirstPersonSubject() {
        mPage               = InspectorPage::Selection;
        mFocusSubjectSearch = true;
    }

private:
    void drawRail(PanelContext const& ctx, float width, float height);
    void drawSelectionPage(PanelContext const& ctx);
    void drawExportPage(PanelContext const& ctx);
    void drawSettingsPage(PanelContext const& ctx);
    void drawOptiPistonSection();

    // Each returns true when it owns the current selection and has drawn it.
    bool drawWorldActor(PanelContext const& ctx);
    bool drawWorldActorSegment(PanelContext const& ctx);
    bool drawSubActor(PanelContext const& ctx);
    bool drawCamera(PanelContext const& ctx);
    bool drawKeyframes(PanelContext const& ctx);
    bool drawKeyframe(PanelContext const& ctx);
    bool drawMarker(PanelContext const& ctx);
    bool drawSequenceSegment(PanelContext const& ctx);
    void drawOverview(PanelContext const& ctx);

    InspectorPage        mPage{InspectorPage::Selection};
    std::optional<float> mPistonDurationDraft;
    bool                 mFocusSubjectSearch{false};
    // The label field edits a local copy so typing is not overwritten by the project each frame.
    std::string           mMarkerLabelId;
    std::array<char, 128> mMarkerLabelBuffer{};
    bool                  mMarkerLabelEditing{};
};

} // namespace playback::editor::ui
