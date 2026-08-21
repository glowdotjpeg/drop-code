#pragma once
#include <Windows.h>

namespace dc::monitor {

struct PanelFrames {
    RECT shown;
    RECT hidden;
};

HMONITOR MonitorForCursor();

RECT MonitorRect(HMONITOR monitor);

PanelFrames FramesFor(HMONITOR monitor, double heightRatio);

}
