#pragma once

// The native upscaler has one history/context. Admit only a single stable scene
// camera until it disappears; otherwise alternating cameras invalidate history.
class PrimaryCamera
{
    int selectedId_ = 0;
    int lastSeenFrame_ = -1;
    bool selected_ = false;
public:
    bool Allow(int cameraId, int frame)
    {
        if (!selected_ || frame < lastSeenFrame_ || frame - lastSeenFrame_ > 1) {
            selected_ = true;
            selectedId_ = cameraId;
        }
        if (cameraId != selectedId_) return false;
        if (frame > lastSeenFrame_) lastSeenFrame_ = frame;
        return true;
    }
    void Reset() { selected_ = false; selectedId_ = 0; lastSeenFrame_ = -1; }
};
