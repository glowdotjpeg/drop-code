#include "settings.h"

#include <CommCtrl.h>
#include <dwmapi.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <cmath>
#include <string>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "resource.h"

namespace dc::settings {
namespace {

using winrt::Microsoft::UI::Xaml::Controls::ComboBox;
using winrt::Microsoft::UI::Xaml::Controls::InfoBar;
using winrt::Microsoft::UI::Xaml::Controls::Slider;
using winrt::Microsoft::UI::Xaml::Controls::TextBox;
using winrt::Microsoft::UI::Xaml::Controls::Button;
using winrt::Microsoft::UI::Xaml::ElementTheme;
using winrt::Microsoft::UI::Xaml::FrameworkElement;
using winrt::Microsoft::UI::Xaml::FocusState;
using winrt::Microsoft::UI::Xaml::UIElement;
using winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource;
using winrt::Microsoft::UI::Xaml::Hosting::XamlSourceFocusNavigationReason;
using winrt::Microsoft::UI::Xaml::Hosting::XamlSourceFocusNavigationRequest;

constexpr const wchar_t* kSettingsWindowClass = L"DropCodeSettingsWindow";
constexpr DWORD kSettingsWindowStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME |
    WS_CLIPCHILDREN;
constexpr DWORD kSettingsWindowExStyle = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;

constexpr int kLauncherPresetCount = 4;
constexpr const wchar_t* kPresets[] = {L"OpenCode", L"OpenCode v2", L"Codex",
                                       L"Claude", L"Custom"};
constexpr const wchar_t* kPresetCommands[] = {L"opencode", L"opencode2",
                                                L"codex", L"claude", nullptr};

UINT DpiForWindow(HWND hwnd) {
    if (hwnd) {
        const UINT dpi = GetDpiForWindow(hwnd);
        if (dpi != 0) return dpi;
    }
    return 96;
}

RECT WindowRectForClient(int clientWidth, int clientHeight, UINT dpi) {
    RECT rect{0, 0, MulDiv(clientWidth, static_cast<int>(dpi), 96),
              MulDiv(clientHeight, static_cast<int>(dpi), 96)};
    if (!AdjustWindowRectExForDpi(&rect, kSettingsWindowStyle, FALSE,
                                  kSettingsWindowExStyle, dpi)) {
        AdjustWindowRectEx(&rect, kSettingsWindowStyle, FALSE,
                           kSettingsWindowExStyle);
    }
    return rect;
}

std::wstring LoadXamlResource(HINSTANCE instance) {
    const HRSRC resource = FindResourceW(
        instance, MAKEINTRESOURCEW(IDR_SETTINGS_XAML), RT_RCDATA);
    if (!resource) return {};

    const HGLOBAL loaded = LoadResource(instance, resource);
    const DWORD byteCount = SizeofResource(instance, resource);
    const auto* bytes = static_cast<const char*>(LockResource(loaded));
    if (!loaded || byteCount == 0 || !bytes) return {};

    int offset = 0;
    if (byteCount >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }

    const int characterCount = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset,
        static_cast<int>(byteCount) - offset, nullptr, 0);
    if (characterCount <= 0) return {};

    std::wstring result(static_cast<size_t>(characterCount), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + offset,
                            static_cast<int>(byteCount) - offset,
                            result.data(), characterCount) <= 0) {
        return {};
    }
    return result;
}

template <typename T>
T FindControl(const FrameworkElement& root, const wchar_t* name) {
    const auto value = root.FindName(name);
    if (!value) {
        throw winrt::hresult_error(E_FAIL);
    }
    return value.as<T>();
}

void SetIslandWindowStyle(const DesktopWindowXamlSource& island) {
    const HWND islandWindow =
        winrt::Microsoft::UI::GetWindowFromWindowId(
            island.SiteBridge().WindowId());
    if (!islandWindow) return;
    SetWindowLongPtrW(islandWindow, GWL_STYLE,
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP);
    SetWindowLongPtrW(islandWindow, GWL_EXSTYLE, 0);
}

void ApplyNativeTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;

    const BOOL useDarkMode = dark ? TRUE : FALSE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(hwnd, kUseImmersiveDarkMode,
                                     &useDarkMode, sizeof(useDarkMode)))) {
        constexpr DWORD kLegacyUseImmersiveDarkMode = 19;
        DwmSetWindowAttribute(hwnd, kLegacyUseImmersiveDarkMode,
                              &useDarkMode, sizeof(useDarkMode));
    }
}

}

struct SettingsWindow::Impl {
    DesktopWindowXamlSource island{nullptr};
    UIElement root{nullptr};
    FrameworkElement rootElement{nullptr};

    Slider heightSlider{nullptr};
    Slider opacitySlider{nullptr};
    ComboBox launcherCombo{nullptr};
    TextBox commandBox{nullptr};
    TextBox workingDirectoryBox{nullptr};
    Button browseButton{nullptr};
    Button applyButton{nullptr};
    ComboBox themeCombo{nullptr};
    InfoBar statusInfoBar{nullptr};

    winrt::event_token heightChanged{};
    winrt::event_token opacityChanged{};
    winrt::event_token launcherChanged{};
    winrt::event_token browseClicked{};
    winrt::event_token applyClicked{};
    winrt::event_token themeChanged{};
    winrt::event_token takeFocusRequested{};
    bool eventsConnected = false;
    bool syncing = false;
};

SettingsWindow::SettingsWindow() : impl_(std::make_unique<Impl>()) {}

SettingsWindow::~SettingsWindow() {
    Shutdown();
}

bool SettingsWindow::IsOpen() const {
    return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE;
}

void SettingsWindow::Show(HINSTANCE instance, HWND owner, const PanelApi& api) {
    instance_ = instance;
    owner_ = owner;
    api_ = api;

    if (hwnd_) {
        SyncValues();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        LayoutIsland();
        return;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(
        static_cast<INT_PTR>(COLOR_WINDOW + 1));
    wc.lpszClassName = kSettingsWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    const RECT initialRect = WindowRectForClient(
        620, 700, DpiForWindow(owner));
    hwnd_ = CreateWindowExW(
        kSettingsWindowExStyle, kSettingsWindowClass, L"DropCode settings",
        kSettingsWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        initialRect.right - initialRect.left,
        initialRect.bottom - initialRect.top, owner, nullptr, instance, this);
    if (!hwnd_) return;

    try {
        if (!InitializeXaml()) {
            DestroyWindow(hwnd_);
            return;
        }
    } catch (const winrt::hresult_error&) {
        ReleaseXaml();
        DestroyWindow(hwnd_);
        return;
    } catch (...) {
        ReleaseXaml();
        DestroyWindow(hwnd_);
        return;
    }

    RECT windowRect{};
    GetWindowRect(hwnd_, &windowRect);
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    const HMONITOR monitor = MonitorFromWindow(
        owner ? owner : hwnd_, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = monitorInfo.rcWork.left +
                      ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) -
                       width) /
                          2;
        const int y = monitorInfo.rcWork.top +
                      ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) -
                       height) /
                          2;
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    LayoutIsland();
    if (impl_->island) {
        impl_->island.NavigateFocus(
            XamlSourceFocusNavigationRequest(XamlSourceFocusNavigationReason::First));
    }
}

void SettingsWindow::Close() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_HIDE);
    if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
}

void SettingsWindow::Shutdown() {
    ReleaseXaml();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    impl_ = std::make_unique<Impl>();
}

bool SettingsWindow::InitializeXaml() {
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (impl_->island) return true;

    impl_->island = DesktopWindowXamlSource();
    impl_->island.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd_));
    impl_->island.ShouldConstrainPopupsToWorkArea(true);
    SetIslandWindowStyle(impl_->island);

    const std::wstring xaml = LoadXamlResource(instance_);
    if (xaml.empty()) return false;

    impl_->root = winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
                       winrt::hstring(xaml.c_str()))
                      .as<UIElement>();
    impl_->rootElement = impl_->root.as<FrameworkElement>();
    impl_->island.Content(impl_->root);
    BindControls();
    SyncValues();
    LayoutIsland();
    return true;
}

void SettingsWindow::ReleaseXaml() {
    if (!impl_) return;

    if (impl_->eventsConnected) {
        impl_->heightSlider.ValueChanged(impl_->heightChanged);
        impl_->opacitySlider.ValueChanged(impl_->opacityChanged);
        impl_->launcherCombo.SelectionChanged(impl_->launcherChanged);
        impl_->browseButton.Click(impl_->browseClicked);
        impl_->applyButton.Click(impl_->applyClicked);
        impl_->themeCombo.SelectionChanged(impl_->themeChanged);
        impl_->island.TakeFocusRequested(impl_->takeFocusRequested);
        impl_->eventsConnected = false;
    }

    try {
        if (impl_->island) {
            impl_->island.Content(nullptr);
            impl_->island.Close();
        }
    } catch (...) {
    }

    impl_->statusInfoBar = nullptr;
    impl_->themeCombo = nullptr;
    impl_->applyButton = nullptr;
    impl_->browseButton = nullptr;
    impl_->workingDirectoryBox = nullptr;
    impl_->commandBox = nullptr;
    impl_->launcherCombo = nullptr;
    impl_->opacitySlider = nullptr;
    impl_->heightSlider = nullptr;
    impl_->rootElement = nullptr;
    impl_->root = nullptr;
    impl_->island = nullptr;
}

void SettingsWindow::BindControls() {
    impl_->heightSlider = FindControl<Slider>(impl_->rootElement, L"HeightSlider");
    impl_->opacitySlider =
        FindControl<Slider>(impl_->rootElement, L"OpacitySlider");
    impl_->launcherCombo =
        FindControl<ComboBox>(impl_->rootElement, L"LauncherCombo");
    impl_->commandBox =
        FindControl<TextBox>(impl_->rootElement, L"CommandBox");
    impl_->workingDirectoryBox =
        FindControl<TextBox>(impl_->rootElement, L"WorkingDirectoryBox");
    impl_->browseButton =
        FindControl<Button>(impl_->rootElement, L"BrowseButton");
    impl_->applyButton =
        FindControl<Button>(impl_->rootElement, L"ApplyButton");
    impl_->themeCombo =
        FindControl<ComboBox>(impl_->rootElement, L"ThemeCombo");
    impl_->statusInfoBar =
        FindControl<InfoBar>(impl_->rootElement, L"StatusInfoBar");

    impl_->heightChanged = impl_->heightSlider.ValueChanged(
        [this](auto const&, auto const& args) {
            if (impl_->syncing) return;
            if (api_.setHeight) {
                api_.setHeight(static_cast<int>(std::lround(args.NewValue())));
            }
        });
    impl_->opacityChanged = impl_->opacitySlider.ValueChanged(
        [this](auto const&, auto const& args) {
            if (impl_->syncing) return;
            if (api_.setOpacity) {
                api_.setOpacity(static_cast<int>(std::lround(args.NewValue())));
            }
        });
    impl_->launcherChanged = impl_->launcherCombo.SelectionChanged(
        [this](auto const&, auto const&) {
            if (impl_->syncing) return;
            const int index = impl_->launcherCombo.SelectedIndex();
            if (index >= 0 && index < kLauncherPresetCount) {
                impl_->commandBox.Text(kPresetCommands[index]);
            } else {
                impl_->commandBox.Focus(FocusState::Programmatic);
            }
        });
    impl_->browseClicked = impl_->browseButton.Click(
        [this](auto const&, auto const&) { BrowseForDirectory(); });
    impl_->applyClicked = impl_->applyButton.Click(
        [this](auto const&, auto const&) { Apply(); });
    impl_->themeChanged = impl_->themeCombo.SelectionChanged(
        [this](auto const&, auto const&) {
            if (impl_->syncing) return;
            const int index = std::clamp(impl_->themeCombo.SelectedIndex(),
                                         0, 2);
            const auto preference =
                static_cast<registry::ThemePreference>(index);
            if (api_.setTheme) api_.setTheme(preference);
            ApplyTheme(preference);
        });
    impl_->takeFocusRequested = impl_->island.TakeFocusRequested(
        [this](auto const&, auto const&) {
            if (hwnd_) SetFocus(hwnd_);
        });
    impl_->eventsConnected = true;
}

void SettingsWindow::LayoutIsland() {
    if (!hwnd_ || !impl_ || !impl_->island) return;

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int width = std::max(0L, client.right - client.left);
    const int height = std::max(0L, client.bottom - client.top);
    impl_->island.SiteBridge().MoveAndResize({0, 0, width, height});
}

void SettingsWindow::SyncValues() {
    if (!impl_ || !impl_->island || !impl_->heightSlider) return;

    impl_->syncing = true;
    const int height = api_.getHeight ? api_.getHeight() : 40;
    const int opacity = api_.getOpacity ? api_.getOpacity() : 90;
    impl_->heightSlider.Value(static_cast<double>(std::clamp(height, 20, 100)));
    impl_->opacitySlider.Value(
        static_cast<double>(std::clamp(opacity, 0, 100)));

    const std::wstring command =
        api_.getCommand ? api_.getCommand() : std::wstring{};
    const std::wstring directory = api_.getWorkingDirectory
                                       ? api_.getWorkingDirectory()
                                       : std::wstring{};
    impl_->commandBox.Text(command.c_str());
    impl_->workingDirectoryBox.Text(directory.c_str());

    int presetIndex = kLauncherPresetCount;
    for (int i = 0; i < kLauncherPresetCount; ++i) {
        if (command == kPresetCommands[i]) {
            presetIndex = i;
            break;
        }
    }
    impl_->launcherCombo.SelectedIndex(presetIndex);

    const auto preference = api_.getTheme
                                ? api_.getTheme()
                                : registry::ThemePreference::System;
    impl_->themeCombo.SelectedIndex(std::clamp(
        static_cast<int>(preference), 0, 2));
    ApplyTheme(preference);
    impl_->statusInfoBar.IsOpen(false);
    impl_->syncing = false;
}

void SettingsWindow::Apply() {
    if (!impl_ || !impl_->commandBox || !api_.applySession) return;

    const std::wstring command(impl_->commandBox.Text().c_str());
    const std::wstring directory(
        impl_->workingDirectoryBox.Text().c_str());
    if (!api_.applySession(command, directory)) {
        SetStatus(L"Could not apply settings",
                  L"Check the command and working directory.", true);
        impl_->commandBox.Focus(FocusState::Programmatic);
        MessageBeep(MB_ICONWARNING);
        return;
    }

    SyncValues();
    SetStatus(L"Settings saved", L"The active session is restarting.", false);
}

void SettingsWindow::ApplyTheme(registry::ThemePreference preference) {
    if (impl_ && impl_->rootElement) {
        ElementTheme theme = ElementTheme::Default;
        if (preference == registry::ThemePreference::Light) {
            theme = ElementTheme::Light;
        } else if (preference == registry::ThemePreference::Dark) {
            theme = ElementTheme::Dark;
        }
        impl_->rootElement.RequestedTheme(theme);
        ApplyNativeTheme(hwnd_, registry::IsDarkTheme(preference));
    }
}

void SettingsWindow::SetStatus(const std::wstring& title,
                               const std::wstring& message, bool isError) {
    if (!impl_ || !impl_->statusInfoBar) return;
    impl_->statusInfoBar.Title(title.c_str());
    impl_->statusInfoBar.Message(message.c_str());
    impl_->statusInfoBar.Severity(
        isError ? winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Error
                : winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
    impl_->statusInfoBar.IsOpen(true);
}

void SettingsWindow::BrowseForDirectory() {
    if (!hwnd_ || !impl_ || !impl_->workingDirectoryBox) return;

    IFileDialog* dialog = nullptr;
    const HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return;

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"Choose a working directory");

    const std::wstring current(impl_->workingDirectoryBox.Text().c_str());
    if (!current.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                current.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }

    if (SUCCEEDED(dialog->Show(hwnd_))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dialog->GetResult(&result)) && result) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) &&
                path) {
                impl_->workingDirectoryBox.Text(path);
                CoTaskMemFree(path);
            }
            result->Release();
        }
    }
    dialog->Release();
}

bool SettingsWindow::HandleDialogMessage(MSG* message) {
    if (!message || !hwnd_ || !IsWindowVisible(hwnd_) || !impl_ ||
        !impl_->island) {
        return false;
    }

    if (message->message == WM_KEYDOWN && message->wParam == VK_ESCAPE) {
        Close();
        return true;
    }

    if (message->message != WM_KEYDOWN || message->wParam != VK_TAB) {
        return false;
    }

    const HWND focused = GetFocus();
    const HWND islandWindow = winrt::Microsoft::UI::GetWindowFromWindowId(
        impl_->island.SiteBridge().WindowId());
    if (!focused ||
        (focused != hwnd_ && focused != islandWindow &&
         !IsChild(islandWindow, focused))) {
        return false;
    }

    const auto reason = (GetKeyState(VK_SHIFT) < 0)
                            ? XamlSourceFocusNavigationReason::Last
                            : XamlSourceFocusNavigationReason::First;
    impl_->island.NavigateFocus(XamlSourceFocusNavigationRequest(reason));
    return true;
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            LayoutIsland();
            return 0;

        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (impl_ && impl_->rootElement && api_.getTheme &&
                api_.getTheme() == registry::ThemePreference::System) {
                ApplyTheme(registry::ThemePreference::System);
            }
            return 0;

        case WM_SETFOCUS:
            if (impl_ && impl_->island) {
                impl_->island.NavigateFocus(
                    XamlSourceFocusNavigationRequest(
                        XamlSourceFocusNavigationReason::First));
                return 0;
            }
            break;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            if (!info) return 0;
            const RECT minimum =
                WindowRectForClient(600, 600, DpiForWindow(hwnd_));
            info->ptMinTrackSize.x = minimum.right - minimum.left;
            info->ptMinTrackSize.y = minimum.bottom - minimum.top;
            return 0;
        }

        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            LayoutIsland();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            Close();
            return 0;

        case WM_NCDESTROY:
            {
            const HWND destroyedWindow = hwnd_;
            ReleaseXaml();
            hwnd_ = nullptr;
            return DefWindowProcW(destroyedWindow, msg, wParam, lParam);
            }

        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

}
