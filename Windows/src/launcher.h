#pragma once
#include <string>

namespace dc::launcher {

bool IsValidCommand(const std::wstring& command);
void CleanupStaleScripts();
std::wstring ScriptPath(const std::wstring& launchCommand,
                        unsigned long long sessionId);

std::wstring Directory();

}
