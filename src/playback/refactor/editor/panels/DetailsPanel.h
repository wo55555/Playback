#pragma once

namespace playback::refactor::editor {

class DetailsPanel {
public:
    void draw();

private:
    void drawEmpty();
    void drawSequence();
    void drawSequenceSegment();
    void drawWorldActor();
    void drawWorldActorSegment();
    void drawSubActor();
    void drawCamera();
    void drawKeyframe();
    void drawMarker();
};

}
