#include "launcher.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string ReadFile(const std::wstring& path) {
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

}

int main() {
    const std::wstring first = dc::launcher::ScriptPath(L"opencode", 0xA11CE);
    const std::wstring second = dc::launcher::ScriptPath(L"opencode", 0xA11CE);

    bool passed = true;
    passed &= Expect(!first.empty(), "first launch script was not created");
    passed &= Expect(!second.empty(), "second launch script was not created");
    passed &= Expect(first != second, "launch script paths were not unique");
    passed &= Expect(dc::launcher::ScriptPath(L"opencode\rmalformed", 0xA11CE)
                         .empty(),
                     "invalid launch command unexpectedly created a script");

    if (!first.empty()) {
        const std::string bytes = ReadFile(first);
        constexpr char prefix[] = "@echo off\r\n";
        passed &= Expect(bytes.rfind(prefix, 0) == 0,
                         "launch script does not begin with ASCII @echo off");
        passed &= Expect(bytes.size() < 3 ||
                             static_cast<unsigned char>(bytes[0]) != 0xEF ||
                             static_cast<unsigned char>(bytes[1]) != 0xBB ||
                             static_cast<unsigned char>(bytes[2]) != 0xBF,
                         "launch script contains a UTF-8 BOM");
        passed &= Expect(bytes.find("call opencode\r\n") != std::string::npos,
                         "launch script lost the configured command");
    }

    if (!first.empty()) {
        passed &= Expect(DeleteFileW(first.c_str()) != 0,
                         "first launch script was not deleted");
    }
    if (!second.empty()) {
        passed &= Expect(DeleteFileW(second.c_str()) != 0,
                         "second launch script was not deleted");
    }

    if (passed) std::cout << "PASS: launch scripts are unique and BOM-free\n";
    return passed ? 0 : 1;
}
