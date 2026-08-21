#pragma once
#include <Windows.h>

namespace dc::tray {

constexpr UINT kCallbackMessage = WM_APP + 2;

enum MenuId {
    kMenuToggle = 1001,
    kMenuRestart = 1002,
    kMenuSettings = 1003,
    kMenuQuit = 1004,
};

class Tray {
public:
    Tray() = default;

    bool Create(HWND owner, HINSTANCE instance);
    void Destroy();
    void Restore();
    void ShowMenu();
    bool IsCreated() const { return created_; }
    UINT TaskbarCreatedMessage() const { return taskbarCreatedMessage_; }

private:
    HWND owner_ = nullptr;
    NOTIFYICONDATAW nid_{};
    UINT taskbarCreatedMessage_ = 0;
    bool created_ = false;
};

}
