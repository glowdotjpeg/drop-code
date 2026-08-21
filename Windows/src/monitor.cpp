#include "monitor.h"

namespace dc::monitor {

HMONITOR MonitorForCursor() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    return monitor;
}

RECT MonitorRect(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        return info.rcMonitor;
    }
    RECT rect{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    return rect;
}

PanelFrames FramesFor(HMONITOR monitor, double heightRatio) {
    const RECT screen = MonitorRect(monitor);
    const int height = static_cast<int>(
        static_cast<double>(screen.bottom - screen.top) * heightRatio);

    RECT shown{};
    shown.left = screen.left;
    shown.right = screen.right;
    shown.top = screen.top;
    shown.bottom = screen.top + height;

    RECT hidden = shown;
    hidden.top = screen.top - height;
    hidden.bottom = screen.top;

    return {shown, hidden};
}

}
