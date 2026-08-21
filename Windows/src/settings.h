#pragma once
#include <Windows.h>

#include <functional>
#include <memory>
#include <string>

#include "registry.h"

namespace dc::settings {

struct PanelApi {
    std::function<int()> getHeight;
    std::function<int()> getOpacity;
    std::function<std::wstring()> getCommand;
    std::function<std::wstring()> getWorkingDirectory;
    std::function<registry::ThemePreference()> getTheme;
    std::function<void(int)> setHeight;
    std::function<void(int)> setOpacity;
    std::function<void(registry::ThemePreference)> setTheme;
    std::function<bool(const std::wstring&, const std::wstring&)> applySession;
};

class SettingsWindow {
public:
    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    void Show(HINSTANCE instance, HWND owner, const PanelApi& api);
    void Close();
    void Shutdown();
    bool IsOpen() const;

    bool HandleDialogMessage(MSG* message);

private:
    struct Impl;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    bool InitializeXaml();
    void ReleaseXaml();
    void LayoutIsland();
    void BindControls();
    void SyncValues();
    void Apply();
    void ApplyTheme(registry::ThemePreference preference);
    void SetStatus(const std::wstring& title, const std::wstring& message,
                   bool isError);
    void BrowseForDirectory();

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    PanelApi api_;
    std::unique_ptr<Impl> impl_;
};

}
