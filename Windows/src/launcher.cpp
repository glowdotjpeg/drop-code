#include "launcher.h"

#include <Windows.h>
#include <shlobj.h>

#include <atomic>
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

bool WriteBytes(HANDLE file, const void* data, DWORD size, DWORD& error) {
    DWORD written = 0;
    if (!WriteFile(file, data, size, &written, nullptr)) {
        error = GetLastError();
        return false;
    }
    if (written != size) {
        error = ERROR_WRITE_FAULT;
        return false;
    }
    return true;
}

bool WriteScript(const std::wstring& path, const std::string& utf8,
                 DWORD& error) {
    error = ERROR_SUCCESS;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return false;
    }

    const DWORD scriptBytes = static_cast<DWORD>(utf8.size());
    bool success = WriteBytes(file, utf8.data(), scriptBytes, error);
    if (!CloseHandle(file) && success) {
        error = GetLastError();
        success = false;
    }
    if (!success) DeleteFileW(path.c_str());
    return success;
}

unsigned long long ProcessNonce() {
    static const unsigned long long nonce = [] {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return GetTickCount64() ^
               static_cast<unsigned long long>(counter.QuadPart);
    }();
    return nonce;
}

unsigned long long NextScriptSequence() {
    static std::atomic<unsigned long long> sequence{0};
    return sequence.fetch_add(1, std::memory_order_relaxed);
}

}

bool IsValidCommand(const std::wstring& command) {
    if (command.empty()) return false;
    for (wchar_t ch : command) {
        if (ch < L' ' || ch == 0x7f) return false;
    }
    return true;
}

void CleanupStaleScripts() {
    const std::wstring directory = Directory();
    const std::wstring pattern = directory + L"\\launch-*.cmd";
    WIN32_FIND_DATAW data{};
    const HANDLE search = FindFirstFileW(pattern.c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const std::wstring path = directory + L"\\" + data.cFileName;
            DeleteFileW(path.c_str());
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
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

    const std::string utf8 = Utf8(script);
    if (utf8.empty() ||
        utf8.size() > std::numeric_limits<DWORD>::max()) {
        return {};
    }

    constexpr int kCreateAttempts = 32;
    const DWORD processId = GetCurrentProcessId();
    const unsigned long long processNonce = ProcessNonce();
    for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
        const std::wstring scriptPath =
            dir + L"\\launch-" + std::to_wstring(sessionId) + L"-" +
            std::to_wstring(processId) + L"-" +
            std::to_wstring(processNonce) + L"-" +
            std::to_wstring(NextScriptSequence()) + L".cmd";

        DWORD error = ERROR_SUCCESS;
        if (WriteScript(scriptPath, utf8, error)) return scriptPath;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            return {};
        }
    }
    return {};
}

}
