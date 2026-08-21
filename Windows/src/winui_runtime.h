#pragma once

#include <Windows.h>

#include <functional>

namespace dc::winui {

HRESULT InitializeWindowsAppSdk();
void ShutdownWindowsAppSdk();

int RunXamlApplication(std::function<int()> launchCallback);
void ShutdownXaml();

}
