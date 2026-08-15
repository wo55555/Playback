#pragma once

#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/keyframe/CameraTimelineRegistry.h"

#include <memory>
#include <optional>
#include <string>

namespace playback::editor::exporting {

class ExportKeyframeApplier {
public:
    ExportKeyframeApplier() = default;
    ~ExportKeyframeApplier();

    ExportKeyframeApplier(ExportKeyframeApplier const&)            = delete;
    ExportKeyframeApplier& operator=(ExportKeyframeApplier const&) = delete;

    void configure(
        editing::model::EditorStateExt const& project,
        std::optional<std::string>            cameraFallback = std::nullopt
    );
    void reset();

private:
    keyframe::CameraTimelineHandle mTimeline;
};

} // namespace playback::editor::exporting
