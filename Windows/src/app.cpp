#include "app.h"

#include "registry.h"

namespace dc::app {
namespace {

constexpr const wchar_t* kHiddenClass = L"DropCodeHiddenWindow";
constexpr const wchar_t* kInstanceMutexName =
    L"Local\\DropCode.SingleInstance";
constexpr const wchar_t* kToggleMessageName = L"DropCode.TogglePanel";

}

bool AppController::ActivateExisting() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutexName);
    if (!mutex) return false;
    CloseHandle(mutex);

    const UINT toggleMessage = RegisterWindowMessageW(kToggleMessageName);
    const HWND existing = FindWindowW(kHiddenClass, nullptr);
    if (existing && toggleMessage != 0) {
        PostMessageW(existing, toggleMessage, 0, 0);
    }
    return true;
}

AppController::~AppController() {
    Shutdown();
}

bool AppController::Create(HINSTANCE instance) {
    instance_ = instance;

    instanceMutex_ = CreateMutexW(nullptr, FALSE, kInstanceMutexName);
    if (!instanceMutex_) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
        ActivateExisting();
        return false;
    }
    created_ = true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHiddenClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Shutdown();
        return false;
    }

    toggleMessage_ = RegisterWindowMessageW(kToggleMessageName);
    if (toggleMessage_ == 0) {
        Shutdown();
        return false;
    }

    hiddenWindow_ = CreateWindowExW(0, kHiddenClass, L"DropCode", WS_OVERLAPPED,
                                    0, 0, 0, 0, nullptr, nullptr, instance,
                                    this);
    if (!hiddenWindow_) {
        Shutdown();
        return false;
    }

    if (!panel_.Create(instance) || !tray_.Create(hiddenWindow_, instance)) {
        Shutdown();
        return false;
    }

    return true;
}

void AppController::Shutdown() {
    if (!created_) {
        if (instanceMutex_) {
            CloseHandle(instanceMutex_);
            instanceMutex_ = nullptr;
        }
        return;
    }
    settings_.Shutdown();
    tray_.Destroy();
    panel_.Destroy();
    if (hiddenWindow_) {
        DestroyWindow(hiddenWindow_);
        hiddenWindow_ = nullptr;
    }
    UnregisterClassW(kHiddenClass, instance_);
    if (instanceMutex_) {
        CloseHandle(instanceMutex_);
        instanceMutex_ = nullptr;
    }
    created_ = false;
}

LRESULT CALLBACK AppController::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
    AppController* self =
        reinterpret_cast<AppController*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<AppController*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hiddenWindow_ = hwnd;
    }
    if (self) return self->HandleMessage(msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT AppController::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (toggleMessage_ != 0 && msg == toggleMessage_) {
        panel_.ToggleLatched();
        return 0;
    }

    if (tray_.TaskbarCreatedMessage() != 0 &&
        msg == tray_.TaskbarCreatedMessage()) {
        tray_.Restore();
        return 0;
    }

    switch (msg) {
        case tray::kCallbackMessage: {
            const UINT action = LOWORD(lParam);
            if (action == WM_RBUTTONUP || action == WM_LBUTTONUP) {
                tray_.ShowMenu();
            } else if (action == WM_LBUTTONDBLCLK) {
                panel_.ToggleLatched();
            }
            return 0;
        }

        case WM_COMMAND: {
            const UINT id = LOWORD(wParam);
            switch (id) {
                case tray::kMenuToggle:
                    panel_.ToggleLatched();
                    return 0;
                case tray::kMenuRestart:
                    panel_.RestartTerminal();
                    return 0;
                case tray::kMenuSettings: {
                    settings::PanelApi api;
                    api.getHeight = [this] { return panel_.HeightPercentage(); };
                    api.getOpacity = [this] { return panel_.OpacityPercentage(); };
                    api.getCommand = [this] { return panel_.LaunchCommand(); };
                    api.getWorkingDirectory = [this] {
                        return panel_.WorkingDirectory();
                    };
                    api.getTheme = [] { return registry::SettingsTheme(); };
                    api.setHeight = [this](int v) { panel_.SetHeightPercentage(v); };
                    api.setOpacity = [this](int v) { panel_.SetOpacityPercentage(v); };
                    api.setTheme = [](registry::ThemePreference preference) {
                        registry::SetSettingsTheme(preference);
                    };
                    api.applySession = [this](const std::wstring& command,
                                              const std::wstring& directory) {
                        return panel_.ApplySessionSettings(command, directory);
                    };
                    settings_.Show(instance_, hiddenWindow_, api);
                    return 0;
                }
                case tray::kMenuQuit:
                    PostMessageW(hiddenWindow_, WM_CLOSE, 0, 0);
                    return 0;
                default:
                    break;
            }
            return 0;
        }

        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hiddenWindow_, msg, wParam, lParam);
}

}
