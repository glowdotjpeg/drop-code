#include <Windows.h>
#include <CommCtrl.h>
#include <shellscalingapi.h>

#include <Microsoft.UI.Dispatching.Interop.h>
#include <winrt/base.h>

#include "app.h"
#include "winui_runtime.h"

namespace {
int RunMessageLoop(HINSTANCE instance, bool settingsAvailable) {
    dc::app::AppController app;
    if (!app.Create(instance, settingsAvailable)) return 1;

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (settingsAvailable && ContentPreTranslateMessage(&msg)) continue;
        if (app.HandleDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

int GuardedRun(HINSTANCE instance) {
    if (dc::app::AppController::ActivateExisting()) return 0;

    const HRESULT bootstrapResult = dc::winui::InitializeWindowsAppSdk();
    if (FAILED(bootstrapResult)) return RunMessageLoop(instance, false);

    int result = 1;
    bool coreStarted = false;
    try {
        result = dc::winui::RunXamlApplication(
            [&] {
                coreStarted = true;
                return RunMessageLoop(instance, true);
            });
    } catch (...) {
        dc::winui::ShutdownWindowsAppSdk();
        return coreStarted ? 1 : RunMessageLoop(instance, false);
    }
    dc::winui::ShutdownWindowsAppSdk();
    return result;
}

int RunWithSeh(HINSTANCE instance) {
    __try {
        return GuardedRun(instance);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    int result = 0;
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        apartmentInitialized = true;
        result = RunWithSeh(instance);
    } catch (...) {
        result = 1;
    }
    if (apartmentInitialized) winrt::uninit_apartment();
    return result;
}
