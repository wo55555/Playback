#pragma once

#include "playback/state/editing/models/EditorStateExt.h"
#include "playback/keyframe/CameraTimelineRegistry.h"

#include <memory>
#include <optional>
#include <string>

namespace playback::exporting {

class ExportKeyframeApplier {
public:
    ExportKeyframeApplier() = default;
    ~ExportKeyframeApplier();

    ExportKeyframeApplier(ExportKeyframeApplier const&)            = delete;
    ExportKeyframeApplier& operator=(ExportKeyframeApplier const&) = delete;

    void configure(
        state::editing::model::EditorStateExt const& project,
        std::optional<std::string>            cameraFallback = std::nullopt
    );
    void reset();

private:
    keyframe::CameraTimelineHandle mTimeline;
};

} // namespace playback::exporting
