#pragma once
#include <Windows.h>

#include "panel.h"
#include "settings.h"
#include "tray.h"

namespace dc::app {

class AppController {
public:
    AppController() = default;
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    static bool ActivateExisting();
    bool Create(HINSTANCE instance, bool settingsAvailable);
    bool HandleDialogMessage(MSG* message) {
        return settingsAvailable_ && settings_.HandleDialogMessage(message);
    }

private:
    void Shutdown();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    HINSTANCE instance_ = nullptr;
    HANDLE instanceMutex_ = nullptr;
    HWND hiddenWindow_ = nullptr;
    dc::panel::Panel panel_;
    tray::Tray tray_;
    settings::SettingsWindow settings_;
    UINT toggleMessage_ = 0;
    bool created_ = false;
    bool settingsAvailable_ = true;
};

}
