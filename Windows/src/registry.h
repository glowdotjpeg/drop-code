#pragma once
#include <string>

namespace dc::registry {

constexpr int kDefaultHeightPercentage = 40;
constexpr int kDefaultOpacityPercentage = 90;
constexpr const wchar_t* kDefaultLaunchCommand = L"opencode";

enum class ThemePreference {
    System = 0,
    Light = 1,
    Dark = 2,
};

int HeightPercentage();
int OpacityPercentage();
std::wstring LaunchCommand();
std::wstring DefaultWorkingDirectory();
std::wstring WorkingDirectory();
ThemePreference SettingsTheme();
bool SystemUsesDarkTheme();
bool IsDarkTheme(ThemePreference preference);

bool SetHeightPercentage(int value);
bool SetOpacityPercentage(int value);
bool SetLaunchCommand(const std::wstring& command);
bool SetWorkingDirectory(const std::wstring& directory);
bool SetSettingsTheme(ThemePreference preference);

}
