#include "launcher.h"

#include <Windows.h>
#include <shlobj.h>

#include <limits>

namespace dc::launcher {
namespace {

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};

    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

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
    const std::string utf8 = Utf8(script);
    if (utf8.empty() ||
        utf8.size() > (std::numeric_limits<DWORD>::max() - 3ULL)) {
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    constexpr unsigned char kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    const DWORD scriptBytes = static_cast<DWORD>(utf8.size());
    const bool wroteBom = WriteFile(file, kUtf8Bom, sizeof(kUtf8Bom), &written,
                                    nullptr) &&
                          written == sizeof(kUtf8Bom);
    const bool wroteScript = wroteBom &&
                             WriteFile(file, utf8.data(), scriptBytes, &written,
                                       nullptr) &&
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
        L"echo DropCode command exited (%exit_code%).\r\n"
        L"endlocal & exit /b %exit_code%\r\n";

    const std::wstring scriptPath = dir + L"\\launch-" +
                                     std::to_wstring(sessionId) + L".cmd";
    return WriteScript(scriptPath, script) ? scriptPath : std::wstring{};
}

}
