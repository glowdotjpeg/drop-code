#include "panel.h"

#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <windowsx.h>

#include "launcher.h"
#include "monitor.h"
#include "registry.h"

namespace dc::panel {
namespace {

constexpr const wchar_t* kWindowClass = L"DropCodePanelWindow";
constexpr const wchar_t* kTabBarClass = L"DropCodeTabBarWindow";
constexpr UINT kTerminalUpdateMessage = WM_APP + 1;
constexpr UINT kTabStartedMessage = WM_APP + 3;
constexpr UINT kTabFailedMessage = WM_APP + 4;
constexpr UINT kTabExitedMessage = WM_APP + 5;
constexpr UINT kChordInputReleasedMessage = WM_APP + 6;
constexpr int kTabBarHeight = 36;

float EaseOutCubic(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

LONGLONG AnimationCounter() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double AnimationCounterFrequency() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 1.0;
    }();
    return frequency;
}

UINT AnimationTimerInterval(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    const int refreshRate = dc ? GetDeviceCaps(dc, VREFRESH) : 0;
    if (dc) ReleaseDC(hwnd, dc);
    if (refreshRate <= 0) return kAnimationTimerIntervalMs;
    return static_cast<UINT>(std::clamp(1000 / refreshRate, 1, 16));
}

bool ReduceMotionEnabled() {
    BOOL animations = FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return !animations;
}

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

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(1);
    }
    return false;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboardWithRetry(owner)) return false;

    bool success = false;
    EmptyClipboard();
    const SIZE_T byteCount = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (storage) {
        void* destination = GlobalLock(storage);
        if (destination) {
            CopyMemory(destination, text.c_str(), byteCount);
            GlobalUnlock(storage);
            if (SetClipboardData(CF_UNICODETEXT, storage)) {
                storage = nullptr;
                success = true;
            }
        }
    }
    if (storage) GlobalFree(storage);
    CloseClipboard();
    return success;
}

std::wstring GetClipboardText(HWND owner) {
    if (!OpenClipboardWithRetry(owner)) return {};

    std::wstring result;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            const SIZE_T byteCount = GlobalSize(handle);
            if (text && byteCount >= sizeof(wchar_t)) {
                const size_t characterCount = byteCount / sizeof(wchar_t);
                size_t length = 0;
                while (length < characterCount && text[length] != L'\0') {
                    ++length;
                }
                result.assign(text, length);
            }
            if (text) GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return result;
}

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

void PostPanelMessage(const std::weak_ptr<LifetimeToken>& weakLifetime,
                      UINT message, WPARAM wParam, LPARAM lParam) {
    const std::shared_ptr<LifetimeToken> lifetime = weakLifetime.lock();
    if (!lifetime || !lifetime->alive.load()) return;
    const HWND hwnd = lifetime->hwnd.load();
    if (hwnd) PostMessageW(hwnd, message, wParam, lParam);
}

void PostTerminalInvalidation(
    const std::weak_ptr<LifetimeToken>& weakLifetime,
    const std::shared_ptr<UpdateGate>& updateGate) {
    if (!updateGate) return;
    const std::shared_ptr<LifetimeToken> lifetime = weakLifetime.lock();
    if (!lifetime || !lifetime->alive.load()) return;

    bool expected = false;
    if (!updateGate->updateScheduled.compare_exchange_strong(expected, true)) {
        return;
    }

    const HWND hwnd = lifetime->hwnd.load();
    if (!hwnd || !PostMessageW(hwnd, kTerminalUpdateMessage, 0, 0)) {
        updateGate->updateScheduled.store(false);
    }
}

void StopTerminalAsync(const std::shared_ptr<dc::terminal::Terminal>& terminal) {
    if (!terminal) return;
    std::thread([terminal] { terminal->Stop(); }).detach();
}

}

Panel::Panel()
    : lifetime_(std::make_shared<LifetimeToken>()),
      updateGate_(std::make_shared<UpdateGate>()) {
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
    if (selecting_) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        selecting_ = false;
    }
    chordMonitor_.Stop();
    if (hwnd_) {
        KillTimer(hwnd_, kAnimationTimerId);
        KillTimer(hwnd_, kTerminalFrameTimerId);
    }
    animating_ = false;
    terminalFramePending_ = false;
    updateGate_->updateScheduled.store(false);
    ReleaseAnimationTimerResolution();
    lifetime_->alive.store(false);
    lifetime_->hwnd.store(nullptr);
    for (auto& tab : tabs_) StopTabAsync(tab);
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
            if (!wParam || (ActiveTab() && ActiveTab()->id == wParam)) {
                if (!terminalFramePending_) {
                    terminalFramePending_ = true;
                    if (SetTimer(hwnd_, kTerminalFrameTimerId,
                                 AnimationTimerInterval(hwnd_), nullptr) == 0) {
                        terminalFramePending_ = false;
                        updateGate_->updateScheduled.store(false);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            } else {
                updateGate_->updateScheduled.store(false);
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
            if (!succeeded && tab == ActiveTab()) MessageBeep(MB_ICONERROR);
            InvalidateTabBar();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        case kTabExitedMessage: {
            TabSession* tab = FindTab(static_cast<uint64_t>(wParam));
            const uint64_t generation = static_cast<uint64_t>(lParam);
            if (!tab || tab->generation.load() != generation) return 0;
            if (tab->state.exchange(TabState::Failed) == TabState::Running) {
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
            suppressNextChar_ = false;
            chordMonitor_.OnHotkey();
            return 0;

        case kChordInputReleasedMessage:
            chordInputSuppressed_ = false;
            suppressNextChar_ = false;
            return 0;

        case WM_TIMER:
            if (wParam == kAnimationTimerId) {
                AnimationTick();
            } else if (wParam == hotkey::ChordMonitor::kChordPollTimerId) {
                chordMonitor_.OnPollTick();
            } else if (wParam == kTerminalFrameTimerId) {
                KillTimer(hwnd_, kTerminalFrameTimerId);
                terminalFramePending_ = false;
                updateGate_->updateScheduled.store(false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            HandleKeyDown(static_cast<UINT>(wParam), false);
            return 0;

        case WM_SYSKEYDOWN:
            HandleKeyDown(static_cast<UINT>(wParam), true);
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

        case WM_MOUSEWHEEL:
            HandleMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
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
                ActiveTab()->terminal->SendMouseButton(1, true,
                                                       mods);
            }
            return 0;

        case WM_LBUTTONUP:
            if (selecting_) {
                UpdateSelection(
                    POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                if (GetCapture() == hwnd_) ReleaseCapture();
                selecting_ = false;
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
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                ActiveTab()->terminal->SendMouseButton(1, false,
                                                       CurrentModifiers());
            }
            return 0;

        case WM_RBUTTONDOWN:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    return 0;
                }
                ActiveTab()->terminal->SendMouseButton(2, true, mods);
            }
            return 0;

        case WM_RBUTTONUP:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    PasteClipboard();
                    return 0;
                }
                ActiveTab()->terminal->SendMouseButton(2, false, mods);
            }
            return 0;

        case WM_MBUTTONDOWN:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    return 0;
                }
                ActiveTab()->terminal->SendMouseButton(3, true, mods);
            }
            return 0;

        case WM_MBUTTONUP:
            if (ActiveTab() && ActiveTab()->state.load() == TabState::Running) {
                const VTermModifier mods = CurrentModifiers();
                if ((mods & VTERM_MOD_SHIFT) == 0 &&
                    !ActiveTab()->terminal->MouseReportingEnabled()) {
                    PasteClipboard();
                    return 0;
                }
                ActiveTab()->terminal->SendMouseButton(3, false, mods);
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (selecting_) {
                UpdateSelection(
                    POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
                return 0;
            }
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const renderer::CellSize metrics = renderer_.CellMetrics();
            if (ActiveTab() &&
                ActiveTab()->state.load() == TabState::Running &&
                pt.y >= tabBarHeight_) {
                ActiveTab()->terminal->SendMouseMove(
                    static_cast<int>(std::floor((pt.y - tabBarHeight_) /
                                                metrics.height)),
                    static_cast<int>(std::floor(pt.x / metrics.width)),
                    CurrentModifiers());
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            selecting_ = false;
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
    if (!visible && selecting_) {
        if (GetCapture() == hwnd_) ReleaseCapture();
        selecting_ = false;
    }
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
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        // A hold gesture opens the panel while Ctrl+Alt+T is still down.
        // Activating the terminal at that point lets the chord's translated
        // characters enter the PTY. Tap gestures are finished before this
        // path runs, so they can activate normally.
        if (!chordMonitor_.IsChordDown()) {
            SetForegroundWindow(hwnd_);
            SetFocus(hwnd_);
        }
        BeginAnimation(frames.shown.top, true);
        ResizeTerminal();
    } else {
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

bool Panel::BeginMomentary() {
    if (isVisible_) return false;
    isLatched_ = false;
    SetVisible(true);
    return isVisible_;
}

void Panel::EndMomentary(bool openedByGesture) {
    if (!openedByGesture || !isVisible_ || isLatched_) return;
    SetVisible(false);
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

void Panel::StartTabAsync(const std::shared_ptr<TabSession>& tab,
                          int cols, int rows) {
    if (!tab || !tab->terminal || !hwnd_) return;

    const auto token = std::make_shared<StartToken>();
    uint64_t generation = 0;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        tab->startToken = token;
        generation = tab->generation.fetch_add(1) + 1;
        tab->state.store(TabState::Starting);
    }

    const auto terminal = tab->terminal;
    const uint64_t tabId = tab->id;
    const std::wstring command = launchCommand_;
    const std::wstring directory = tab->workingDirectory;
    const std::weak_ptr<LifetimeToken> weakLifetime = lifetime_;
    const std::shared_ptr<UpdateGate> updateGate = updateGate_;
    terminal->SetInvalidateCallback([weakLifetime, updateGate] {
        PostTerminalInvalidation(weakLifetime, updateGate);
    });
    terminal->SetExitCallback([weakLifetime, tabId, generation] {
        PostPanelMessage(weakLifetime, kTabExitedMessage,
                         static_cast<WPARAM>(tabId),
                         static_cast<LPARAM>(generation));
    });

    try {
        std::thread([tab, token, terminal, command, directory, cols, rows,
                     tabId, generation, weakLifetime] {
            bool started = false;
            try {
                started = terminal->Start(command, directory, cols, rows, tabId);
            } catch (...) {
                terminal->Stop();
            }

            bool stale = false;
            {
                std::lock_guard lock(tab->lifecycleMutex);
                stale = token->cancelled.load() ||
                        tab->generation.load() != generation;
                if (!stale) {
                    tab->state.store(started ? TabState::Running
                                              : TabState::Failed);
                }
            }

            if (stale) {
                terminal->Stop();
                return;
            }

            PostPanelMessage(weakLifetime,
                             started ? kTabStartedMessage : kTabFailedMessage,
                             static_cast<WPARAM>(tabId),
                             static_cast<LPARAM>(generation));
        }).detach();
    } catch (...) {
        std::lock_guard lock(tab->lifecycleMutex);
        if (tab->generation.load() == generation && tab->startToken == token) {
            token->cancelled.store(true);
            tab->state.store(TabState::Failed);
            InvalidateTabBar();
            InvalidateRect(hwnd_, nullptr, FALSE);
            MessageBeep(MB_ICONERROR);
        }
    }
}

void Panel::StopTabAsync(const std::shared_ptr<TabSession>& tab) {
    if (!tab) return;

    std::shared_ptr<dc::terminal::Terminal> terminal;
    TabState state = TabState::Stopping;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        if (tab->startToken) tab->startToken->cancelled.store(true);
        state = tab->state.exchange(TabState::Stopping);
        terminal = tab->terminal;
    }

    if (state == TabState::Running) StopTerminalAsync(terminal);
}

Panel::TabSession* Panel::ActiveTab() {
    if (activeTab_ >= tabs_.size()) return nullptr;
    return tabs_[activeTab_].get();
}

const Panel::TabSession* Panel::ActiveTab() const {
    if (activeTab_ >= tabs_.size()) return nullptr;
    return tabs_[activeTab_].get();
}

void Panel::NewTab() {
    if (!hwnd_) return;

    auto tab = std::make_shared<TabSession>();
    tab->id = nextTabId_++;
    tab->workingDirectory = workingDirectory_;
    tab->terminal = std::make_shared<dc::terminal::Terminal>();

    tabs_.push_back(tab);
    activeTab_ = tabs_.size() - 1;
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    StartTabAsync(tab, renderer_.ColsForWidth(client.right - client.left),
                  renderer_.RowsForHeight(terminalHeight));
}

void Panel::CloseActiveTab() {
    if (tabs_.size() <= 1) {
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    const auto tab = tabs_[activeTab_];
    StopTabAsync(tab);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(activeTab_));
    if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.size() - 1;
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);
}

void Panel::SelectTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    activeTab_ = static_cast<size_t>(index);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);
}

void Panel::SelectRelativeTab(int direction) {
    if (tabs_.size() <= 1) return;
    const int count = static_cast<int>(tabs_.size());
    int next = static_cast<int>(activeTab_) + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    SelectTab(next);
}

void Panel::RestartActiveTab() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    std::shared_ptr<dc::terminal::Terminal> oldTerminal;
    TabState oldState = TabState::Stopping;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        if (tab->startToken) tab->startToken->cancelled.store(true);
        oldState = tab->state.load();
        oldTerminal = tab->terminal;
        tab->terminal = std::make_shared<dc::terminal::Terminal>();
        tab->state.store(TabState::Starting);
    }
    if (oldState == TabState::Running) StopTerminalAsync(oldTerminal);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    const int rows = renderer_.RowsForHeight(terminalHeight);
    const int cols = renderer_.ColsForWidth(client.right - client.left);
    StartTabAsync(tabs_[activeTab_], cols, rows);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
}

Panel::TabSession* Panel::FindTab(uint64_t tabId) {
    for (const auto& tab : tabs_) {
        if (tab && tab->id == tabId) return tab.get();
    }
    return nullptr;
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

void Panel::OnTapToggle() {
    ToggleLatched();
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

bool Panel::OnHoldStart() {
    return BeginMomentary();
}

void Panel::OnHoldEnd(bool opened) {
    EndMomentary(opened);
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

void Panel::OnCancel() {
    if (isVisible_ && !isLatched_) SetVisible(false);
    if (hwnd_) PostMessageW(hwnd_, kChordInputReleasedMessage, 0, 0);
}

void Panel::BeginAnimation(int toY, bool entering) {
    KillTimer(hwnd_, kAnimationTimerId);

    RECT current{};
    GetWindowRect(hwnd_, &current);
    const int fromY = current.top;
    if (ReduceMotionEnabled() || fromY == toY) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, toY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        if (!entering) {
            ShowWindow(hwnd_, SW_HIDE);
            RestorePreviousForeground();
        }
        animating_ = false;
        ReleaseAnimationTimerResolution();
        return;
    }
    animating_ = true;
    animEntering_ = entering;
    animStartY_ = fromY;
    animEndY_ = toY;
    animLeft_ = current.left;
    animStartCounter_ = AnimationCounter();
    if (!animationTimerResolutionActive_ &&
        timeBeginPeriod(1) == TIMERR_NOERROR) {
        animationTimerResolutionActive_ = true;
    }
    if (SetTimer(hwnd_, kAnimationTimerId, AnimationTimerInterval(hwnd_),
                 nullptr) == 0) {
        animating_ = false;
        SetWindowPos(hwnd_, HWND_TOPMOST, current.left, toY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        if (!entering) {
            ShowWindow(hwnd_, SW_HIDE);
            RestorePreviousForeground();
        }
        ReleaseAnimationTimerResolution();
        return;
    }
    AnimationTick();
}

void Panel::AnimationTick() {
    if (!animating_) return;

    const LONGLONG elapsedCounter =
        std::max<LONGLONG>(0, AnimationCounter() - animStartCounter_);
    const double elapsedMs =
        (static_cast<double>(elapsedCounter) / AnimationCounterFrequency()) * 1000.0;
    const float t = std::min(
        1.0f, static_cast<float>(elapsedMs / kAnimationDurationMs));
    const float eased = EaseOutCubic(t);
    const int y = static_cast<int>(std::lround(
        animStartY_ + (animEndY_ - animStartY_) * eased));

    SetWindowPos(hwnd_, nullptr, animLeft_, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);

    if (t >= 1.0f) {
        KillTimer(hwnd_, kAnimationTimerId);
        animating_ = false;
        if (!animEntering_) {
            ShowWindow(hwnd_, SW_HIDE);
            RestorePreviousForeground();
        }
        ReleaseAnimationTimerResolution();
    }
}

void Panel::ReleaseAnimationTimerResolution() {
    if (!animationTimerResolutionActive_) return;
    timeEndPeriod(1);
    animationTimerResolutionActive_ = false;
}

void Panel::ApplyOpacity() {
    if (!hwnd_) return;
    SetLayeredWindowAttributes(hwnd_, 0,
                               static_cast<BYTE>(opacityPercent_ * 255 / 100),
                               LWA_ALPHA);
}

void Panel::ResizeTerminal() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    const int rows = renderer_.RowsForHeight(terminalHeight);
    const int cols = renderer_.ColsForWidth(client.right - client.left);
    for (auto& tab : tabs_) {
        if (tab && tab->terminal) tab->terminal->Resize(cols, rows);
    }
}

void Panel::RestorePreviousForeground() {
    if (previousForeground_ && IsWindow(previousForeground_)) {
        SetForegroundWindow(previousForeground_);
    }
    previousForeground_ = nullptr;
}

VTermModifier Panel::CurrentModifiers() const {
    VTermModifier mods = VTERM_MOD_NONE;
    if (GetKeyState(VK_SHIFT) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_SHIFT);
    }
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_CTRL);
    }
    if (GetKeyState(VK_MENU) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_ALT);
    }
    return mods;
}

void Panel::HandleKeyDown(UINT vk, bool sysKey) {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;
    auto& terminal = *tab->terminal;

    // Do not feed the global open chord into the terminal if focus was
    // already transferred before Windows finished dispatching its messages.
    if (chordInputSuppressed_ || chordMonitor_.IsChordDown()) return;

    const VTermModifier mods = CurrentModifiers();
    const bool ctrl = (mods & VTERM_MOD_CTRL) != 0;
    const bool alt = (mods & VTERM_MOD_ALT) != 0;
    const bool shift = (mods & VTERM_MOD_SHIFT) != 0;

    if (ctrl && !alt && vk == 'C' &&
        (tab->selection.active || shift)) {
        CopySelection();
        suppressNextChar_ = true;
        return;
    }
    if (ctrl && !alt && vk == 'V') {
        PasteClipboard();
        suppressNextChar_ = true;
        return;
    }
    if (!ctrl && !alt && shift && vk == VK_INSERT) {
        PasteClipboard();
        suppressNextChar_ = true;
        return;
    }
    if (ctrl && !alt && vk == VK_INSERT && tab->selection.active) {
        CopySelection();
        suppressNextChar_ = true;
        return;
    }
    if (ctrl && !alt && shift && vk == 'A' &&
        tab->state.load() == TabState::Running) {
        SelectAll();
        suppressNextChar_ = true;
        return;
    }

    if (ctrl && !alt) {
        if (vk == 'T') {
            NewTab();
            suppressNextChar_ = true;
            return;
        }
        if (vk == 'W') {
            CloseActiveTab();
            suppressNextChar_ = true;
            return;
        }
        if (vk == VK_TAB) {
            SelectRelativeTab(shift ? -1 : 1);
            suppressNextChar_ = true;
            return;
        }
        if (vk >= '1' && vk <= '9') {
            SelectTab(static_cast<int>(vk - '1'));
            suppressNextChar_ = true;
            return;
        }
        if (vk == '0') {
            if (!tabs_.empty()) {
                SelectTab(static_cast<int>(tabs_.size() - 1));
            }
            suppressNextChar_ = true;
            return;
        }
    }

    if (tab->state.load() != TabState::Running) return;

    const bool modifierOnly =
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN;
    if (!modifierOnly && tab->selection.active) {
        tab->selection.active = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (terminal.ScrollOffset() > 0) {
        terminal.ResetScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool handled = false;

    if (ctrl && !alt) {
        if (vk >= 'A' && vk <= 'Z') {
            terminal.SendUnichar(static_cast<uint32_t>('a' + (vk - 'A')),
                                 VTERM_MOD_CTRL);
            handled = true;
        } else if (vk >= '0' && vk <= '9') {
            terminal.SendUnichar(vk, VTERM_MOD_CTRL);
            handled = true;
        } else if (vk == VK_SPACE) {
            terminal.SendUnichar(' ', VTERM_MOD_CTRL);
            handled = true;
        }
    }

    if (!handled) {
        switch (vk) {
            case VK_RETURN:
                terminal.SendKey(VTERM_KEY_ENTER, mods);
                handled = true;
                break;
            case VK_TAB:
                terminal.SendKey(VTERM_KEY_TAB, mods);
                handled = true;
                break;
            case VK_BACK:
                terminal.SendKey(VTERM_KEY_BACKSPACE, mods);
                handled = true;
                break;
            case VK_ESCAPE:
                terminal.SendKey(VTERM_KEY_ESCAPE, mods);
                handled = true;
                break;
            case VK_UP:
                terminal.SendKey(VTERM_KEY_UP, mods);
                handled = true;
                break;
            case VK_DOWN:
                terminal.SendKey(VTERM_KEY_DOWN, mods);
                handled = true;
                break;
            case VK_LEFT:
                terminal.SendKey(VTERM_KEY_LEFT, mods);
                handled = true;
                break;
            case VK_RIGHT:
                terminal.SendKey(VTERM_KEY_RIGHT, mods);
                handled = true;
                break;
            case VK_INSERT:
                terminal.SendKey(VTERM_KEY_INS, mods);
                handled = true;
                break;
            case VK_DELETE:
                terminal.SendKey(VTERM_KEY_DEL, mods);
                handled = true;
                break;
            case VK_HOME:
                terminal.SendKey(VTERM_KEY_HOME, mods);
                handled = true;
                break;
            case VK_END:
                terminal.SendKey(VTERM_KEY_END, mods);
                handled = true;
                break;
            case VK_PRIOR:
                terminal.SendKey(VTERM_KEY_PAGEUP, mods);
                handled = true;
                break;
            case VK_NEXT:
                terminal.SendKey(VTERM_KEY_PAGEDOWN, mods);
                handled = true;
                break;
            case VK_F1:
            case VK_F2:
            case VK_F3:
            case VK_F4:
            case VK_F5:
            case VK_F6:
            case VK_F7:
            case VK_F8:
            case VK_F9:
            case VK_F10:
            case VK_F11:
            case VK_F12:
                terminal.SendKey(
                    static_cast<VTermKey>(VTERM_KEY_FUNCTION(vk - VK_F1 + 1)),
                    mods);
                handled = true;
                break;
            default:
                if (sysKey && alt && !ctrl && vk >= 'A' && vk <= 'Z') {
                    terminal.SendUnichar(static_cast<uint32_t>('a' + (vk - 'A')),
                                         VTERM_MOD_ALT);
                    handled = true;
                }
                break;
        }
    }

    suppressNextChar_ = handled &&
                        (vk == VK_RETURN || vk == VK_TAB || vk == VK_BACK ||
                         vk == VK_ESCAPE || (ctrl && !alt) || (sysKey && alt && !ctrl));
}

void Panel::HandleChar(wchar_t ch) {
    if (suppressNextChar_) {
        suppressNextChar_ = false;
        return;
    }
    if (chordInputSuppressed_ || chordMonitor_.IsChordDown()) return;
    TabSession* tab = ActiveTab();
    if (ch == 0x03 && tab && tab->terminal && tab->selection.active &&
        (CurrentModifiers() & VTERM_MOD_CTRL) != 0) {
        CopySelection();
        return;
    }
    if (!tab || !tab->terminal || tab->state.load() != TabState::Running) {
        pendingHighSurrogate_ = 0;
        return;
    }
    auto& terminal = *tab->terminal;
    if (terminal.ScrollOffset() > 0) {
        terminal.ResetScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    uint32_t codepoint = 0;
    if (pendingHighSurrogate_ != 0) {
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            codepoint =
                0x10000 + ((pendingHighSurrogate_ - 0xD800) << 10) + (ch - 0xDC00);
        }
        pendingHighSurrogate_ = 0;
    } else if (ch >= 0xD800 && ch <= 0xDBFF) {
        pendingHighSurrogate_ = ch;
        return;
    } else {
        codepoint = ch;
    }
    if (codepoint == 0) return;

    VTermModifier mods = CurrentModifiers();
    const bool altGr = (mods & VTERM_MOD_CTRL) != 0 &&
                       (GetKeyState(VK_RMENU) & 0x8000) != 0;
    if (altGr) {
        mods = VTERM_MOD_NONE;
    } else {
        mods = static_cast<VTermModifier>(mods & ~VTERM_MOD_CTRL);
    }
    terminal.SendUnichar(codepoint, mods);
}

void Panel::HandleMouseWheel(short delta) {
    const int lines = delta / WHEEL_DELTA;
    TabSession* tab = ActiveTab();
    if (lines != 0 && tab && tab->terminal &&
        tab->state.load() == TabState::Running) {
        if (tab->selection.active) {
            tab->selection.active = false;
        }
        tab->terminal->Scroll(lines);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Panel::BeginSelection(POINT point) {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    const dc::terminal::SelectionPoint cell = CellPointFromClient(point);
    tab->selection.anchor = cell;
    tab->selection.focus = cell;
    tab->selection.active = false;
    selecting_ = true;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::UpdateSelection(POINT point) {
    if (!selecting_) return;
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    const dc::terminal::SelectionPoint cell = CellPointFromClient(point);
    if (tab->selection.focus.row == cell.row &&
        tab->selection.focus.col == cell.col) {
        return;
    }
    tab->selection.focus = cell;
    tab->selection.active =
        tab->selection.anchor.row != tab->selection.focus.row ||
        tab->selection.anchor.col != tab->selection.focus.col;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

dc::terminal::SelectionPoint Panel::CellPointFromClient(POINT point) const {
    dc::terminal::SelectionPoint result{};
    const TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return result;

    const renderer::CellSize metrics = renderer_.CellMetrics();
    const float cellWidth = std::max(1.0f, metrics.width);
    const float cellHeight = std::max(1.0f, metrics.height);
    const int rows = std::max(1, tab->terminal->Rows());
    const int cols = std::max(1, tab->terminal->Cols());

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = std::max(1L, client.right - client.left);
    const int height = std::max(1L, client.bottom - client.top);
    const int x = std::clamp(static_cast<int>(point.x), 0, width - 1);
    const int terminalY = std::clamp(static_cast<int>(point.y) - tabBarHeight_, 0,
                                     std::max(0, height - tabBarHeight_ - 1));

    result.col = std::clamp(static_cast<int>(std::floor(x / cellWidth)), 0,
                            cols - 1);
    result.row = std::clamp(
        static_cast<int>(std::floor(terminalY / cellHeight)), 0, rows - 1);
    return result;
}

bool Panel::CopySelection() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal || !tab->selection.active) return false;

    const std::wstring text = tab->terminal->CopySelection(tab->selection);
    if (text.empty()) return false;
    const bool copied = SetClipboardText(hwnd_, text);
    if (!copied) MessageBeep(MB_ICONWARNING);
    return copied;
}

bool Panel::PasteClipboard() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal || tab->state.load() != TabState::Running) {
        return false;
    }

    const std::wstring text = GetClipboardText(hwnd_);
    if (text.empty()) return false;

    tab->selection.active = false;
    if (tab->terminal->ScrollOffset() > 0) tab->terminal->ResetScroll();
    tab->terminal->SendPaste(text);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Panel::SelectAll() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    const int rows = tab->terminal->Rows();
    const int cols = tab->terminal->Cols();
    if (rows <= 0 || cols <= 0) return;
    tab->selection.anchor = {0, 0};
    tab->selection.focus = {rows - 1, cols - 1};
    tab->selection.active = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}
