#include "launcher.h"

#include <Windows.h>
#include <shlobj.h>

#include <limits>

namespace dc::launcher {
namespace {

std::wstring EscapeBatch(const std::wstring& command) {
    std::wstring escaped;
    escaped.reserve(command.size() + 8);
    for (wchar_t ch : command) {
        switch (ch) {
            case L'%':
                escaped += L"%%";
                break;
            case L'^':
            case L'&':
            case L'|':
            case L'<':
            case L'>':
            case L'(':
            case L')':
                escaped += L'^';
                escaped += ch;
                break;
            default:
                escaped += ch;
        }
    }
    return escaped;
}

bool WriteScript(const std::wstring& path, const std::wstring& script) {
    if (script.size() > (std::numeric_limits<DWORD>::max() / sizeof(wchar_t))) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    const wchar_t bom = static_cast<wchar_t>(0xFEFF);
    DWORD written = 0;
    const DWORD scriptBytes = static_cast<DWORD>(script.size() * sizeof(wchar_t));
    const bool wroteBom = WriteFile(file, &bom, sizeof(bom), &written, nullptr) &&
                          written == sizeof(bom);
    const bool wroteScript =
        wroteBom && WriteFile(file, script.data(), scriptBytes, &written, nullptr) &&
        written == scriptBytes;
    CloseHandle(file);
    return wroteScript;
}

}

bool IsValidCommand(const std::wstring& command) {
    if (command.empty()) return false;
    for (wchar_t ch : command) {
        if (ch < L' ' || ch == 0x7f) return false;
    }
    return true;
}

std::wstring Directory() {
    wchar_t* base = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base) != S_OK) {
        wchar_t expanded[MAX_PATH] = {};
        constexpr DWORD capacity = sizeof(expanded) / sizeof(expanded[0]);
        const DWORD length = ExpandEnvironmentStringsW(
            L"%LOCALAPPDATA%\\DropCode", expanded,
            capacity);
        if (length > 0 && length < capacity) return expanded;
        return L".\\DropCode";
    }
    std::wstring dir = base;
    CoTaskMemFree(base);
    dir += L"\\DropCode";
    return dir;
}

std::wstring ScriptPath(const std::wstring& launchCommand,
                        unsigned long long sessionId) {
    if (!IsValidCommand(launchCommand)) return {};

    std::wstring dir = Directory();
    if (dir.empty() ||
        (!CreateDirectoryW(dir.c_str(), nullptr) &&
         GetLastError() != ERROR_ALREADY_EXISTS)) {
        return {};
    }

    std::wstring escaped = EscapeBatch(launchCommand);
    std::wstring script =
        L"@echo off\r\n"
        L"setlocal DisableDelayedExpansion\r\n"
        L"set \"PATH=%USERPROFILE%\\.local\\bin;%USERPROFILE%\\.bun\\bin;%APPDATA%\\npm;%USERPROFILE%\\scoop\\shims;%PATH%\"\r\n"
        L"call " + escaped +
        L"\r\n"
        L"set \"exit_code=%errorlevel%\"\r\n"
        L"echo.\r\n"
        L"echo DropCode command exited (%exit_code%). Starting a shell.\r\n"
        L"cmd\r\n"
        L"exit /b %exit_code%\r\n";

    const std::wstring scriptPath = dir + L"\\launch-" +
                                     std::to_wstring(sessionId) + L".cmd";
    return WriteScript(scriptPath, script) ? scriptPath : std::wstring{};
}

}
