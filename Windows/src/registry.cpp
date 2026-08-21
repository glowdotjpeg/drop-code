#include "registry.h"

#include <Windows.h>

#include <algorithm>

namespace dc::registry {
namespace {

constexpr const wchar_t* kKeyPath = L"Software\\DropCode";
constexpr const wchar_t* kPersonalizeKeyPath =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

DWORD ReadDword(const wchar_t* name, DWORD defaultValue) {
    DWORD value = defaultValue;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_DWORD,
                     nullptr, &value, &size) != ERROR_SUCCESS) {
        return defaultValue;
    }
    return value;
}

std::wstring ReadString(const wchar_t* name, const std::wstring& defaultValue) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_SZ,
                     nullptr, nullptr, &size) != ERROR_SUCCESS) {
        return defaultValue;
    }
    if (size < sizeof(wchar_t)) return defaultValue;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kKeyPath, name, RRF_RT_REG_SZ,
                     nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return defaultValue;
    }
    const size_t characterCount = size / sizeof(wchar_t);
    if (characterCount == 0) return defaultValue;
    value.resize(characterCount - 1);
    return value;
}

bool WriteDword(const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, kKeyPath, name, REG_DWORD,
                           &value, sizeof(value)) == ERROR_SUCCESS;
}

bool WriteString(const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(
               HKEY_CURRENT_USER, kKeyPath, name, REG_SZ, value.c_str(),
               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
           ERROR_SUCCESS;
}

}

int HeightPercentage() {
    return static_cast<int>(std::clamp(
        ReadDword(L"panelHeightPercentage", kDefaultHeightPercentage),
        static_cast<DWORD>(20), static_cast<DWORD>(100)));
}

int OpacityPercentage() {
    return static_cast<int>(std::min(
        ReadDword(L"backgroundOpacityPercentage", kDefaultOpacityPercentage),
        static_cast<DWORD>(100)));
}

std::wstring LaunchCommand() {
    return ReadString(L"launchCommand", kDefaultLaunchCommand);
}

std::wstring DefaultWorkingDirectory() {
    wchar_t path[32768] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"USERPROFILE", path, static_cast<DWORD>(std::size(path)));
    if (length > 0 && length < std::size(path)) return path;
    return L".";
}

std::wstring WorkingDirectory() {
    return ReadString(L"workingDirectory", DefaultWorkingDirectory());
}

ThemePreference SettingsTheme() {
    constexpr DWORD kUnset = 0xffffffffu;
    const DWORD stored = ReadDword(L"settingsTheme", kUnset);
    if (stored <= static_cast<DWORD>(ThemePreference::Dark)) {
        return static_cast<ThemePreference>(stored);
    }

    if (ReadDword(L"settingsDarkTheme", 0) != 0) {
        return ThemePreference::Dark;
    }
    return ThemePreference::System;
}

bool SystemUsesDarkTheme() {
    DWORD appsUseLightTheme = 1;
    DWORD size = sizeof(appsUseLightTheme);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKeyPath,
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                     &appsUseLightTheme, &size) != ERROR_SUCCESS) {
        return false;
    }
    return appsUseLightTheme == 0;
}

bool IsDarkTheme(ThemePreference preference) {
    switch (preference) {
        case ThemePreference::Dark:
            return true;
        case ThemePreference::Light:
            return false;
        case ThemePreference::System:
        default:
            return SystemUsesDarkTheme();
    }
}

bool SetHeightPercentage(int value) {
    return WriteDword(L"panelHeightPercentage", static_cast<DWORD>(value));
}

bool SetOpacityPercentage(int value) {
    return WriteDword(L"backgroundOpacityPercentage", static_cast<DWORD>(value));
}

bool SetLaunchCommand(const std::wstring& command) {
    return WriteString(L"launchCommand", command);
}

bool SetWorkingDirectory(const std::wstring& directory) {
    return WriteString(L"workingDirectory", directory);
}

bool SetSettingsTheme(ThemePreference preference) {
    DWORD value = static_cast<DWORD>(ThemePreference::System);
    if (preference == ThemePreference::Light) {
        value = static_cast<DWORD>(ThemePreference::Light);
    } else if (preference == ThemePreference::Dark) {
        value = static_cast<DWORD>(ThemePreference::Dark);
    }
    return WriteDword(L"settingsTheme", value);
}

}
