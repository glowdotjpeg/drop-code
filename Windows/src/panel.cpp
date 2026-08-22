#include "panel.h"

#include <algorithm>
#include <iterator>
#include <windowsx.h>

#include "launcher.h"
#include "monitor.h"
#include "registry.h"

namespace dc::panel {
namespace {

bool IsDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring TrimWhitespace(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring NormalizeDirectory(const std::wstring& value) {
    std::wstring path = TrimWhitespace(value);
    if (path.empty()) return {};
    if (!IsDirectory(path)) return {};

    wchar_t fullPath[32768] = {};
    const DWORD length = GetFullPathNameW(
        path.c_str(), static_cast<DWORD>(std::size(fullPath)), fullPath, nullptr);
    if (length == 0 || length >= std::size(fullPath)) return {};
    return fullPath;
}

}

Panel::Panel()
    : lifetime_(std::make_shared<LifetimeToken>()) {
    heightPercent_ = std::clamp(registry::HeightPercentage(),
                                static_cast<int>(kMinHeightPercent),
                                static_cast<int>(kMaxHeightPercent));
    opacityPercent_ = std::clamp(registry::OpacityPercentage(), 0, 100);
    launchCommand_ = TrimWhitespace(registry::LaunchCommand());
    if (!launcher::IsValidCommand(launchCommand_)) {
        launchCommand_ = registry::kDefaultLaunchCommand;
    }
    workingDirectory_ = NormalizeDirectory(registry::WorkingDirectory());
    if (workingDirectory_.empty()) {
        workingDirectory_ = NormalizeDirectory(registry::DefaultWorkingDirectory());
    }
}

Panel::~Panel() {
    Destroy();
}

bool Panel::Create(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    wc.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    const HMONITOR monitor = monitor::MonitorForCursor();
    const monitor::PanelFrames frames =
        monitor::FramesFor(monitor, heightPercent_ / 100.0);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kWindowClass, L"DropCode", WS_POPUP | WS_CLIPCHILDREN,
        frames.hidden.left, frames.hidden.top,
        frames.hidden.right - frames.hidden.left,
        frames.hidden.bottom - frames.hidden.top,
        nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    lifetime_->alive.store(true);
    lifetime_->hwnd.store(hwnd_);

    ApplyOpacity();

    if (!CreateTabBar()) {
        Destroy();
        return false;
    }

    if (!renderer_.Initialize(hwnd_)) {
        Destroy();
        return false;
    }

    launcher::CleanupStaleScripts();
    auto tab = std::make_shared<TabSession>();
    tab->id = nextTabId_++;
    tab->workingDirectory = workingDirectory_;
    tab->terminal = std::make_shared<dc::terminal::Terminal>();
    tabs_.push_back(tab);
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    StartTabAsync(tab, renderer_.ColsForWidth(client.right - client.left),
                  renderer_.RowsForHeight(terminalHeight));
    ResizeTabBar();

    if (!chordMonitor_.Start(hwnd_, *this)) {
        Destroy();
        return false;
    }
    return true;
}

void Panel::Destroy() {
    CancelPointerInteraction();
    chordMonitor_.Stop();
    StopAnimationClock();
    animating_ = false;
    lifetime_->alive.store(false);
    lifetime_->hwnd.store(nullptr);
    for (auto& tab : tabs_) StopTabAsync(tab);
    ReapWorkers(true);
    tabs_.clear();
    activeTab_ = 0;
    renderer_.Shutdown();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    tabBar_ = nullptr;
    UnregisterClassW(kTabBarClass, instance_);
    UnregisterClassW(kWindowClass, instance_);
}

LRESULT CALLBACK Panel::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Panel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Panel::TabBarWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam) {
    Panel* self = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Panel*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->tabBar_ = hwnd;
    }
    if (self) return self->HandleTabBarMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Panel::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCDESTROY:
            hwnd_ = nullptr;
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            TabSession* tab = ActiveTab();
            if (tab && tab->terminal &&
                !renderer_.Render(*tab->terminal, tabBarHeight_,
                                  &tab->selection)) {
                renderer_.Shutdown();
                if (renderer_.Initialize(hwnd_)) {
                    if (tab && tab->terminal) {
                        renderer_.Render(*tab->terminal, tabBarHeight_,
                                         &tab->selection);
                    }
                }
            }
            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            ResizeTabBar();
            renderer_.HandleResize(hwnd_);
            ResizeTerminal();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case kTerminalUpdateMessage:
            if (TabSession* updated = FindTab(static_cast<uint64_t>(wParam))) {
                const uint64_t generation = static_cast<uint64_t>(lParam);
                if (updated->generation.load() != generation) return 0;
                if (updated == ActiveTab() && updated->updateGate) {
                    updated->updateGate->updateScheduled.store(false);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;

        case kTabStartedMessage:
        case kTabFailedMessage: {
            TabSession* tab = FindTab(static_cast<uint64_t>(wParam));
            const uint64_t generation = static_cast<uint64_t>(lParam);
            if (!tab || tab->generation.load() != generation) return 0;
            const bool succeeded = msg == kTabStartedMessage;
            if (succeeded && tab->state.load() == TabState::Failed) return 0;
            tab->state.store(succeeded ? TabState::Running : TabState::Failed);
            if (succeeded && tab == ActiveTab()) {
                ResizeTerminal();
                if (GetFocus() == hwnd_) tab->terminal->SendFocus(true);
            }
            if (!succeeded && tab == ActiveTab()) MessageBeep(MB_ICONERROR);
            InvalidateTabBar();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case kTabExitedMessage: {
            TabSession* tab = FindTab(static_cast<uint64_t>(wParam));
            const uint64_t generation = static_cast<uint64_t>(lParam);
            if (!tab || tab->generation.load() != generation) return 0;
            const TabState previous = tab->state.exchange(TabState::Failed);
            if (previous == TabState::Starting ||
                previous == TabState::Running) {
                StopTerminalAsync(tab->terminal);
                if (tab == ActiveTab()) MessageBeep(MB_ICONERROR);
                InvalidateTabBar();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_HOTKEY:
            // A translated key message can already be queued when the
            // gesture callback runs. Keep the open chord out of the PTY until
            // the gesture has completed and this message queue catches up.
            chordInputSuppressed_ = true;
            translatedCharPending_ = false;
            chordMonitor_.OnHotkey();
            return 0;

        case kChordInputReleasedMessage:
            chordInputSuppressed_ = false;
            translatedCharPending_ = false;
            return 0;

        case WM_TIMER:
            if (wParam == hotkey::ChordMonitor::kChordPollTimerId) {
                chordMonitor_.OnPollTick();
            }
            return 0;

        case kAnimationFrameMessage:
            animationFramePending_.store(false);
            AnimationTick();
            return 0;

        case WM_KEYDOWN:
            HandleKeyDown(static_cast<UINT>(wParam));
            return 0;

        case WM_SYSKEYDOWN:
            HandleKeyDown(static_cast<UINT>(wParam));
            return 0;

        case WM_CHAR:
        case WM_SYSCHAR:
            HandleChar(static_cast<wchar_t>(wParam));
            return 0;

        case WM_COPY:
            CopySelection();
            return 0;

        case WM_PASTE:
            PasteClipboard();
            return 0;

        case WM_SETFOCUS:
            if (TabSession* tab = ActiveTab();
                tab && tab->terminal &&
                tab->state.load() == TabState::Running) {
                tab->terminal->SendFocus(true);
            }
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;

        case WM_KILLFOCUS:
            CancelApplicationMousePress();
            if (TabSession* tab = ActiveTab();
                tab && tab->terminal &&
                tab->state.load() == TabState::Running) {
                tab->terminal->SendFocus(false);
            }
            return 0;

        case WM_MOUSEWHEEL:
            HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam), lParam);
            return 0;

        case WM_LBUTTONDOWN:
            SetFocus(hwnd_);
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                if (ActiveTab()->selection.active) {
                    ActiveTab()->selection.active = false;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                const VTermModifier mods = CurrentModifiers();
                const bool forceSelection =
                    (mods & VTERM_MOD_SHIFT) != 0;
                if (GET_Y_LPARAM(lParam) >= tabBarHeight_ &&
                    (forceSelection ||
                     !ActiveTab()->terminal->MouseReportingEnabled())) {
                    BeginSelection(
                        POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                    return 0;
                }
                BeginApplicationMousePress(
                    1, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, mods);
            }
            return 0;

        case WM_LBUTTONUP:
            if (selecting_) {
                UpdateSelection(
                    POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                selecting_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                TabSession* tab = ActiveTab();
                if (tab && tab->selection.anchor.row ==
                               tab->selection.focus.row &&
                    tab->selection.anchor.col ==
                        tab->selection.focus.col) {
                    tab->selection.active = false;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            EndApplicationMousePress(
                1, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;

        case WM_RBUTTONDOWN:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    return 0;
                }
                BeginApplicationMousePress(
                    2, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, mods);
            }
            return 0;

        case WM_RBUTTONUP:
            if (EndApplicationMousePress(
                    2, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
                return 0;
            }
            if (applicationMousePress_.terminal) return 0;
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    PasteClipboard();
                    return 0;
                }
            }
            return 0;

        case WM_MBUTTONDOWN:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    return 0;
                }
                BeginApplicationMousePress(
                    3, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, mods);
            }
            return 0;

        case WM_MBUTTONUP:
            if (EndApplicationMousePress(
                    3, POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) {
                return 0;
            }
            if (applicationMousePress_.terminal) return 0;
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    PasteClipboard();
                    return 0;
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (selecting_) {
                UpdateSelection(
                    POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (applicationMousePress_.terminal) {
                UpdateApplicationMousePress(pt);
                return 0;
            }
            if (ActiveTab() &&
                ActiveTab()->state.load() == TabState::Running &&
                IsTerminalClientPoint(pt)) {
                const dc::terminal::SelectionPoint cell =
                    CellPointFromClient(pt);
                ActiveTab()->terminal->SendMouseMove(
                    cell.row, cell.col, CurrentModifiers());
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            selecting_ = false;
            CancelApplicationMousePress();
            return 0;

        case WM_CANCELMODE:
            CancelPointerInteraction();
            return 0;

        case WM_CLOSE:
            if (isVisible_) SetVisible(false);
            return 0;

        case WM_DISPLAYCHANGE: {
            const HMONITOR monitor = monitor::MonitorForCursor();
            const monitor::PanelFrames frames =
                monitor::FramesFor(monitor, heightPercent_ / 100.0);
            SetWindowPos(hwnd_, HWND_TOPMOST,
                         isVisible_ ? frames.shown.left : frames.hidden.left,
                         isVisible_ ? frames.shown.top : frames.hidden.top,
                         frames.hidden.right - frames.hidden.left,
                         frames.hidden.bottom - frames.hidden.top,
                         SWP_NOACTIVATE);
            ResizeTabBar();
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, HWND_TOPMOST, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOACTIVATE);
            tabBarHeight_ = TabBarHeightForWindow();
            ResizeTabBar();
            renderer_.Shutdown();
            renderer_.Initialize(hwnd_);
            ResizeTerminal();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void Panel::SetVisible(bool visible) {
    if (visible == isVisible_) return;
    if (!visible) CancelPointerInteraction();
    isVisible_ = visible;

    const HMONITOR monitor = monitor::MonitorForCursor();
    const monitor::PanelFrames frames =
        monitor::FramesFor(monitor, heightPercent_ / 100.0);

    if (visible) {
        if (!animating_ || !previousForeground_) {
            previousForeground_ = GetForegroundWindow();
        }
        if (!animating_) {
            SetWindowPos(hwnd_, HWND_TOPMOST, frames.hidden.left, frames.hidden.top,
                         frames.hidden.right - frames.hidden.left,
                         frames.hidden.bottom - frames.hidden.top,
                         SWP_NOACTIVATE);
        } else {
            RECT current{};
            GetWindowRect(hwnd_, &current);
            SetWindowPos(hwnd_, HWND_TOPMOST, current.left, current.top,
                         frames.hidden.right - frames.hidden.left,
                         frames.hidden.bottom - frames.hidden.top,
                         SWP_NOACTIVATE);
        }
        ShowWindow(hwnd_, chordMonitor_.IsChordDown() ? SW_SHOWNOACTIVATE
                                                       : SW_SHOW);
        // A hold gesture opens the panel while Ctrl+Alt+T is still down.
        // Activating the terminal at that point lets the chord's translated
        // characters enter the PTY. Tap gestures are finished before this
        // path runs, so they can activate normally.
        if (!chordMonitor_.IsChordDown()) {
            SetForegroundWindow(hwnd_);
            SetActiveWindow(hwnd_);
            SetFocus(hwnd_);
        }
        BeginAnimation(frames.shown.top, true);
        ResizeTerminal();
    } else {
        if (TabSession* tab = ActiveTab()) tab->wheelDeltaRemainder = 0;
        RestorePreviousForeground();
        BeginAnimation(frames.hidden.top, false);
    }
}

void Panel::ToggleLatched() {
    if (isVisible_) {
        isLatched_ = false;
        SetVisible(false);
    } else {
        isLatched_ = true;
        SetVisible(true);
    }
}

void Panel::RestartTerminal() {
    RestartActiveTab();
}

void Panel::SetHeightPercentage(int percent) {
    heightPercent_ = std::clamp(percent, static_cast<int>(kMinHeightPercent),
                                static_cast<int>(kMaxHeightPercent));
    registry::SetHeightPercentage(heightPercent_);
    const HMONITOR monitor = monitor::MonitorForCursor();
    const monitor::PanelFrames frames =
        monitor::FramesFor(monitor, heightPercent_ / 100.0);
    SetWindowPos(hwnd_, HWND_TOPMOST,
                 isVisible_ ? frames.shown.left : frames.hidden.left,
                 isVisible_ ? frames.shown.top : frames.hidden.top,
                 frames.hidden.right - frames.hidden.left,
                 frames.hidden.bottom - frames.hidden.top, SWP_NOACTIVATE);
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::SetOpacityPercentage(int percent) {
    opacityPercent_ = std::clamp(percent, 0, 100);
    registry::SetOpacityPercentage(opacityPercent_);
    ApplyOpacity();
}

bool Panel::ApplySessionSettings(const std::wstring& command,
                                  const std::wstring& directory) {
    const std::wstring trimmedCommand = TrimWhitespace(command);
    const std::wstring normalizedDirectory = NormalizeDirectory(directory);
    if (!launcher::IsValidCommand(trimmedCommand) ||
        normalizedDirectory.empty()) {
        return false;
    }

    if (!registry::SetLaunchCommand(trimmedCommand) ||
        !registry::SetWorkingDirectory(normalizedDirectory)) {
        return false;
    }
    launchCommand_ = trimmedCommand;
    workingDirectory_ = normalizedDirectory;

    if (TabSession* tab = ActiveTab()) {
        tab->workingDirectory = workingDirectory_;
    }
    RestartActiveTab();
    InvalidateTabBar();
    return true;
}

void Panel::OnTapToggle() {
    ToggleLatched();
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

bool Panel::OnHoldStart() {
    if (isVisible_) return false;
    isLatched_ = false;
    SetVisible(true);
    return isVisible_;
}

void Panel::OnHoldEnd(bool opened) {
    if (opened && isVisible_ && !isLatched_) SetVisible(false);
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

void Panel::OnCancel() {
    if (isVisible_ && !isLatched_) SetVisible(false);
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

}
