#include "tray.h"

namespace dc::tray {
namespace {

constexpr UINT kTrayIconId = 1;

}

bool Tray::Create(HWND owner, HINSTANCE instance) {
    Destroy();
    owner_ = owner;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = owner;
    nid_.uID = kTrayIconId;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kCallbackMessage;
    nid_.hIcon = LoadIconW(instance, MAKEINTRESOURCE(1));
    wcscpy_s(nid_.szTip, L"DropCode");

    Restore();
    return created_;
}

void Tray::Restore() {
    if (!owner_) return;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    if (!created_) {
        nid_.uFlags = NIF_MESSAGE | NIF_TIP;
        created_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
    }
}

void Tray::Destroy() {
    if (created_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        created_ = false;
    }
    owner_ = nullptr;
    taskbarCreatedMessage_ = 0;
}

void Tray::ShowMenu() {
    if (!owner_) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuToggle, L"Toggle DropCode");
    AppendMenuW(menu, MF_STRING, kMenuRestart, L"Restart OpenCode");
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit DropCode");

    SetMenuDefaultItem(menu, kMenuToggle, FALSE);
    SetForegroundWindow(owner_);

    POINT pt{};
    GetCursorPos(&pt);
    UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, owner_, nullptr);
    DestroyMenu(menu);

    if (id != 0) {
        PostMessageW(owner_, WM_COMMAND, id, 0);
    }
}

}
