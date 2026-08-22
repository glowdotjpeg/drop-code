#include "panel.h"

#include <algorithm>
#include <windowsx.h>

namespace dc::panel {
namespace {

constexpr int kTabBarHeight = 36;

std::wstring DirectoryLabel(const std::wstring& directory) {
    if (directory.size() <= 3 && directory.size() >= 2 &&
        directory[1] == L':') {
        return directory;
    }

    size_t end = directory.find_last_not_of(L"\\/");
    if (end == std::wstring::npos) return directory;
    const size_t separator = directory.find_last_of(L"\\/", end);
    if (separator == std::wstring::npos) return directory.substr(0, end + 1);
    if (separator == 1 && directory[1] == L':') return directory.substr(0, 3);
    return directory.substr(separator + 1, end - separator);
}

int ScaleForWindow(HWND hwnd, int logicalPixels) {
    const UINT dpi = hwnd ? std::max<UINT>(96, GetDpiForWindow(hwnd)) : 96;
    return MulDiv(logicalPixels, static_cast<int>(dpi), 96);
}

}

bool Panel::CreateTabBar() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TabBarWndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kTabBarClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    tabBar_ = CreateWindowExW(
        0, kTabBarClass, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwnd_, nullptr, instance_, this);
    if (!tabBar_) return false;
    tabBarHeight_ = TabBarHeightForWindow();
    ResizeTabBar();
    return true;
}

int Panel::TabBarHeightForWindow() const {
    return ScaleForWindow(hwnd_, kTabBarHeight);
}

void Panel::ResizeTabBar() {
    if (!tabBar_ || !hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::min(
        tabBarHeight_, std::max(1, static_cast<int>(client.bottom - client.top)));
    SetWindowPos(tabBar_, nullptr, 0, 0,
                 width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateTabBar();
}

int Panel::TabWidth() const {
    if (!tabBar_ || tabs_.empty()) return 0;
    RECT client{};
    GetClientRect(tabBar_, &client);
    const RECT plus = PlusButtonRect();
    const int margin = ScaleForWindow(tabBar_, 10);
    const int gap = ScaleForWindow(tabBar_, 6);
    const int count = static_cast<int>(tabs_.size());
    const int available = plus.left - margin - gap * std::max(0, count - 1);
    return std::max(1, std::min(240 * ScaleForWindow(tabBar_, 1),
                                available / std::max(1, count)));
}

RECT Panel::TabRect(int index) const {
    RECT result{};
    if (!tabBar_ || index < 0 || index >= static_cast<int>(tabs_.size())) {
        return result;
    }
    RECT client{};
    GetClientRect(tabBar_, &client);
    const int margin = ScaleForWindow(tabBar_, 10);
    const int gap = ScaleForWindow(tabBar_, 6);
    const int width = TabWidth();
    result.left = margin + index * (width + gap);
    result.right = result.left + width;
    result.top = ScaleForWindow(tabBar_, 5);
    result.bottom = client.bottom - ScaleForWindow(tabBar_, 5);
    return result;
}

RECT Panel::PlusButtonRect() const {
    RECT result{};
    if (!tabBar_) return result;
    RECT client{};
    GetClientRect(tabBar_, &client);
    const int margin = ScaleForWindow(tabBar_, 10);
    const int size = ScaleForWindow(tabBar_, 28);
    result.right = client.right - margin;
    result.left = result.right - size;
    result.top = (client.bottom - size) / 2;
    result.bottom = result.top + size;
    return result;
}

std::wstring Panel::TabLabel(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return {};
    std::wstring label;
    switch (tabs_[index]->state.load()) {
        case TabState::Starting:
            label = L"Starting  ";
            break;
        case TabState::Failed:
            label = L"Failed  ";
            break;
        case TabState::Stopping:
            label = L"Stopping  ";
            break;
        case TabState::Running:
            break;
    }
    return std::to_wstring(index + 1) + L"  " + label +
           DirectoryLabel(tabs_[index]->workingDirectory);
}

void Panel::InvalidateTabBar() {
    if (tabBar_) InvalidateRect(tabBar_, nullptr, FALSE);
}

void Panel::HandleTabBarClick(int x, int y) {
    const RECT plus = PlusButtonRect();
    if (PtInRect(&plus, POINT{x, y})) {
        NewTab();
        return;
    }

    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const RECT tab = TabRect(i);
        if (!PtInRect(&tab, POINT{x, y})) continue;
        const int closeWidth = ScaleForWindow(tabBar_, 28);
        if (x >= tab.right - closeWidth) {
            if (i == static_cast<int>(activeTab_)) {
                CloseActiveTab();
            } else if (tabs_.size() > 1) {
                SelectTab(i);
                CloseActiveTab();
            }
        } else {
            SelectTab(i);
        }
        return;
    }
}

LRESULT Panel::HandleTabBarMessage(HWND tabBar, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(tabBar, &ps);
            PaintTabBar(dc);
            EndPaint(tabBar, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            HandleTabBarClick(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        default:
            break;
    }
    return DefWindowProcW(tabBar, msg, wParam, lParam);
}

void Panel::PaintTabBar(HDC dc) {
    if (!dc || !tabBar_) return;
    RECT client{};
    GetClientRect(tabBar_, &client);

    HBRUSH background = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(dc, &client, background);
    DeleteObject(background);

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    const int closeWidth = ScaleForWindow(tabBar_, 28);
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const RECT tab = TabRect(i);
        const bool active = i == static_cast<int>(activeTab_);
        HBRUSH fill = CreateSolidBrush(active ? RGB(34, 35, 39)
                                              : RGB(18, 19, 22));
        FillRect(dc, &tab, fill);
        DeleteObject(fill);

        HBRUSH border = CreateSolidBrush(active ? RGB(60, 62, 68)
                                                : RGB(32, 33, 37));
        FrameRect(dc, &tab, border);
        DeleteObject(border);

        RECT text = tab;
        text.left += ScaleForWindow(tabBar_, 12);
        text.right -= closeWidth;
        SetTextColor(dc, active ? RGB(232, 234, 238) : RGB(158, 161, 168));
        DrawTextW(dc, TabLabel(i).c_str(), -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (active) {
            RECT accent = tab;
            accent.top = accent.bottom - ScaleForWindow(tabBar_, 2);
            HBRUSH accentBrush = CreateSolidBrush(RGB(112, 167, 255));
            FillRect(dc, &accent, accentBrush);
            DeleteObject(accentBrush);
        }
    }

    const RECT plus = PlusButtonRect();
    HBRUSH plusBorder = CreateSolidBrush(RGB(42, 44, 49));
    FrameRect(dc, &plus, plusBorder);
    DeleteObject(plusBorder);

    HPEN plusPen = CreatePen(PS_SOLID, std::max(1, ScaleForWindow(tabBar_, 1)),
                             RGB(190, 194, 202));
    HGDIOBJ oldPen = SelectObject(dc, plusPen);
    const int plusCenterX = (plus.left + plus.right) / 2;
    const int plusCenterY = (plus.top + plus.bottom) / 2;
    const int plusArm = ScaleForWindow(tabBar_, 6);
    MoveToEx(dc, plusCenterX - plusArm, plusCenterY, nullptr);
    LineTo(dc, plusCenterX + plusArm, plusCenterY);
    MoveToEx(dc, plusCenterX, plusCenterY - plusArm, nullptr);
    LineTo(dc, plusCenterX, plusCenterY + plusArm);

    HPEN closePen = CreatePen(PS_SOLID, std::max(1, ScaleForWindow(tabBar_, 1)),
                              RGB(145, 148, 156));
    SelectObject(dc, closePen);
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        const RECT tab = TabRect(i);
        const int centerX = tab.right - closeWidth / 2;
        const int centerY = (tab.top + tab.bottom) / 2;
        const int closeArm = ScaleForWindow(tabBar_, 4);
        MoveToEx(dc, centerX - closeArm, centerY - closeArm, nullptr);
        LineTo(dc, centerX + closeArm, centerY + closeArm);
        MoveToEx(dc, centerX + closeArm, centerY - closeArm, nullptr);
        LineTo(dc, centerX - closeArm, centerY + closeArm);
    }
    SelectObject(dc, oldPen);
    DeleteObject(closePen);
    DeleteObject(plusPen);
    SelectObject(dc, oldFont);
}

}
