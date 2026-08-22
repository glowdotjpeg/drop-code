#include "panel.h"

#include <algorithm>
#include <windowsx.h>

namespace dc::panel {
namespace {

bool OpenClipboardWithRetry(HWND owner) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(owner)) return true;
        Sleep(1);
    }
    return false;
}

bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboardWithRetry(owner)) return false;

    bool success = false;
    EmptyClipboard();
    const SIZE_T byteCount = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (storage) {
        void* destination = GlobalLock(storage);
        if (destination) {
            CopyMemory(destination, text.c_str(), byteCount);
            GlobalUnlock(storage);
            if (SetClipboardData(CF_UNICODETEXT, storage)) {
                storage = nullptr;
                success = true;
            }
        }
    }
    if (storage) GlobalFree(storage);
    CloseClipboard();
    return success;
}

std::wstring GetClipboardText(HWND owner) {
    if (!OpenClipboardWithRetry(owner)) return {};

    std::wstring result;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            const SIZE_T byteCount = GlobalSize(handle);
            if (text && byteCount >= sizeof(wchar_t)) {
                const size_t characterCount = byteCount / sizeof(wchar_t);
                size_t length = 0;
                while (length < characterCount && text[length] != L'\0') {
                    ++length;
                }
                result.assign(text, length);
            }
            if (text) GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return result;
}

}

VTermModifier Panel::CurrentModifiers() const {
    VTermModifier mods = VTERM_MOD_NONE;
    if (GetKeyState(VK_SHIFT) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_SHIFT);
    }
    if (GetKeyState(VK_CONTROL) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_CTRL);
    }
    if (GetKeyState(VK_MENU) & 0x8000) {
        mods = static_cast<VTermModifier>(mods | VTERM_MOD_ALT);
    }
    return mods;
}

void Panel::SuppressTranslatedChar(wchar_t expected) {
    translatedCharPending_ = true;
    expectedTranslatedChar_ = expected;
    translatedCharMessageTime_ = GetMessageTime();
}

void Panel::HandleKeyDown(UINT vk) {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;
    auto& terminal = *tab->terminal;

    // Do not feed the global open chord into the terminal if focus was
    // already transferred before Windows finished dispatching its messages.
    if (chordInputSuppressed_ || chordMonitor_.IsChordDown()) return;

    const VTermModifier mods = CurrentModifiers();
    const bool ctrl = (mods & VTERM_MOD_CTRL) != 0;
    const bool alt = (mods & VTERM_MOD_ALT) != 0;
    const bool shift = (mods & VTERM_MOD_SHIFT) != 0;
    const bool modifierOnly =
        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN;
    if (modifierOnly) return;

    if (ctrl && !alt && vk == 'C' && tab->selection.active) {
        CopySelection();
        SuppressTranslatedChar(0x03);
        return;
    }
    if (ctrl && !alt && vk == 'V') {
        PasteClipboard();
        SuppressTranslatedChar(0x16);
        return;
    }
    if (!ctrl && !alt && shift && vk == VK_INSERT) {
        PasteClipboard();
        return;
    }
    if (ctrl && !alt && vk == VK_INSERT && tab->selection.active) {
        CopySelection();
        return;
    }
    if (ctrl && !alt && shift && vk == 'A' &&
        tab->state.load() == TabState::Running) {
        SelectAll();
        SuppressTranslatedChar(0x01);
        return;
    }

    if (ctrl && !alt) {
        if (vk == 'T') {
            NewTab();
            SuppressTranslatedChar(0x14);
            return;
        }
        if (vk == 'W') {
            CloseActiveTab();
            SuppressTranslatedChar(0x17);
            return;
        }
        if (vk == VK_TAB) {
            SelectRelativeTab(shift ? -1 : 1);
            return;
        }
        if (vk >= '1' && vk <= '9') {
            SelectTab(static_cast<int>(vk - '1'));
            return;
        }
        if (vk == '0') {
            if (!tabs_.empty()) {
                SelectTab(static_cast<int>(tabs_.size() - 1));
            }
            return;
        }
    }

    if (tab->state.load() != TabState::Running) return;

    if (tab->selection.active) {
        tab->selection.active = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    if (terminal.ScrollOffset() > 0) {
        terminal.ResetScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool handled = false;

    if (ctrl && !alt) {
        if (vk >= 'A' && vk <= 'Z') {
            terminal.SendUnichar(static_cast<uint32_t>('a' + (vk - 'A')),
                                 VTERM_MOD_CTRL);
            SuppressTranslatedChar(
                static_cast<wchar_t>(vk - 'A' + 1));
            handled = true;
        } else if (vk >= '0' && vk <= '9') {
            terminal.SendUnichar(vk, VTERM_MOD_CTRL);
            constexpr wchar_t translatedDigits[] = {
                0, 0, 0, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x7F, 0};
            const wchar_t translated = translatedDigits[vk - '0'];
            if (translated != 0 || vk == '2') {
                SuppressTranslatedChar(translated);
            }
            handled = true;
        } else if (vk == VK_SPACE) {
            terminal.SendUnichar(' ', VTERM_MOD_CTRL);
            SuppressTranslatedChar(0);
            handled = true;
        }
    }

    if (!handled) {
        switch (vk) {
            case VK_RETURN:
                terminal.SendKey(VTERM_KEY_ENTER, mods);
                SuppressTranslatedChar(L'\r');
                handled = true;
                break;
            case VK_TAB:
                terminal.SendKey(VTERM_KEY_TAB, mods);
                SuppressTranslatedChar(L'\t');
                handled = true;
                break;
            case VK_BACK:
                terminal.SendKey(VTERM_KEY_BACKSPACE, mods);
                SuppressTranslatedChar(L'\b');
                handled = true;
                break;
            case VK_ESCAPE:
                terminal.SendKey(VTERM_KEY_ESCAPE, mods);
                SuppressTranslatedChar(0x1B);
                handled = true;
                break;
            case VK_UP:
                terminal.SendKey(VTERM_KEY_UP, mods);
                handled = true;
                break;
            case VK_DOWN:
                terminal.SendKey(VTERM_KEY_DOWN, mods);
                handled = true;
                break;
            case VK_LEFT:
                terminal.SendKey(VTERM_KEY_LEFT, mods);
                handled = true;
                break;
            case VK_RIGHT:
                terminal.SendKey(VTERM_KEY_RIGHT, mods);
                handled = true;
                break;
            case VK_INSERT:
                terminal.SendKey(VTERM_KEY_INS, mods);
                handled = true;
                break;
            case VK_DELETE:
                terminal.SendKey(VTERM_KEY_DEL, mods);
                handled = true;
                break;
            case VK_HOME:
                terminal.SendKey(VTERM_KEY_HOME, mods);
                handled = true;
                break;
            case VK_END:
                terminal.SendKey(VTERM_KEY_END, mods);
                handled = true;
                break;
            case VK_PRIOR:
                terminal.SendKey(VTERM_KEY_PAGEUP, mods);
                handled = true;
                break;
            case VK_NEXT:
                terminal.SendKey(VTERM_KEY_PAGEDOWN, mods);
                handled = true;
                break;
            case VK_F1:
            case VK_F2:
            case VK_F3:
            case VK_F4:
            case VK_F5:
            case VK_F6:
            case VK_F7:
            case VK_F8:
            case VK_F9:
            case VK_F10:
            case VK_F11:
            case VK_F12:
                terminal.SendKey(
                    static_cast<VTermKey>(VTERM_KEY_FUNCTION(vk - VK_F1 + 1)),
                    mods);
                handled = true;
                break;
            default:
                break;
        }
    }

}

void Panel::HandleChar(wchar_t ch) {
    if (translatedCharPending_) {
        const bool suppress = ch == expectedTranslatedChar_ &&
                              GetMessageTime() == translatedCharMessageTime_;
        translatedCharPending_ = false;
        if (suppress) return;
    }
    if (chordInputSuppressed_ || chordMonitor_.IsChordDown()) return;
    TabSession* tab = ActiveTab();
    if (ch == 0x03 && tab && tab->terminal && tab->selection.active &&
        (CurrentModifiers() & VTERM_MOD_CTRL) != 0) {
        CopySelection();
        return;
    }
    if (!tab || !tab->terminal || tab->state.load() != TabState::Running) {
        pendingHighSurrogate_ = 0;
        return;
    }
    auto& terminal = *tab->terminal;
    if (terminal.ScrollOffset() > 0) {
        terminal.ResetScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    uint32_t codepoint = 0;
    if (pendingHighSurrogate_ != 0) {
        if (ch >= 0xDC00 && ch <= 0xDFFF) {
            codepoint =
                0x10000 + ((pendingHighSurrogate_ - 0xD800) << 10) + (ch - 0xDC00);
        }
        pendingHighSurrogate_ = 0;
    } else if (ch >= 0xD800 && ch <= 0xDBFF) {
        pendingHighSurrogate_ = ch;
        return;
    } else {
        codepoint = ch;
    }
    if (codepoint == 0) return;

    VTermModifier mods = CurrentModifiers();
    const bool altGr = (mods & VTERM_MOD_CTRL) != 0 &&
                       (GetKeyState(VK_RMENU) & 0x8000) != 0;
    if (altGr) {
        mods = VTERM_MOD_NONE;
    } else {
        mods = static_cast<VTermModifier>(mods & ~VTERM_MOD_CTRL);
    }
    terminal.SendUnichar(codepoint, mods);
}

bool Panel::HandleMouseWheel(short delta, POINT screenPoint,
                             VTermModifier modifiers, bool nonBlocking) {
    TabSession* tab = ActiveTab();
    if (!isVisible_ || !tab || !tab->terminal ||
        tab->state.load() != TabState::Running) {
        if (tab) tab->wheelDeltaRemainder = 0;
        return false;
    }

    POINT point = screenPoint;
    if (!ScreenToClient(hwnd_, &point) || !IsTerminalClientPoint(point)) {
        tab->wheelDeltaRemainder = 0;
        return false;
    }

    int remainder = tab->wheelDeltaRemainder;
    if (remainder != 0 && delta != 0 &&
        (remainder > 0) != (delta > 0)) {
        remainder = 0;
    }
    remainder += static_cast<int>(delta);
    const int notches = remainder / WHEEL_DELTA;
    remainder %= WHEEL_DELTA;
    if (notches == 0) {
        tab->wheelDeltaRemainder = remainder;
        return true;
    }

    const dc::terminal::SelectionPoint cell =
        CellPointForTerminal(point, *tab->terminal);
    const std::optional<dc::terminal::Terminal::WheelRoute> route =
        nonBlocking
            ? tab->terminal->TryRouteWheel(cell.row, cell.col, notches,
                                           modifiers)
            : std::optional{tab->terminal->RouteWheel(
                  cell.row, cell.col, notches, modifiers)};
    if (!route) return false;

    tab->wheelDeltaRemainder = remainder;
    if (*route == dc::terminal::Terminal::WheelRoute::Scrollback &&
        tab->selection.active) {
        tab->selection.active = false;
    }
    if (*route != dc::terminal::Terminal::WheelRoute::Ignored) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    return true;
}

bool Panel::BeginApplicationMousePress(int button, POINT point,
                                       VTermModifier modifiers) {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal || tab->state.load() != TabState::Running ||
        selecting_ || applicationMousePress_.terminal ||
        !IsTerminalClientPoint(point)) {
        return false;
    }

    SetCapture(hwnd_);
    if (GetCapture() != hwnd_) return false;

    const auto terminal = tab->terminal;
    const dc::terminal::SelectionPoint cell =
        CellPointForTerminal(point, *terminal);
    applicationMousePress_.button = button;
    applicationMousePress_.cell = cell;
    applicationMousePress_.terminal = terminal;
    terminal->SendMouseButtonAt(cell.row, cell.col, button, true, modifiers);
    return true;
}

bool Panel::EndApplicationMousePress(int button, POINT point) {
    if (!applicationMousePress_.terminal ||
        applicationMousePress_.button != button) {
        return false;
    }

    const auto terminal = applicationMousePress_.terminal;
    const dc::terminal::SelectionPoint cell =
        CellPointForTerminal(point, *terminal);
    applicationMousePress_.cell = cell;
    applicationMousePress_ = {};
    terminal->SendMouseButtonAt(cell.row, cell.col, button, false,
                                CurrentModifiers());
    if (GetCapture() == hwnd_) ReleaseCapture();
    return true;
}

void Panel::UpdateApplicationMousePress(POINT point) {
    if (!applicationMousePress_.terminal) return;

    const auto terminal = applicationMousePress_.terminal;
    const dc::terminal::SelectionPoint cell =
        CellPointForTerminal(point, *terminal);
    applicationMousePress_.cell = cell;
    terminal->SendMouseMove(cell.row, cell.col, CurrentModifiers());
}

void Panel::CancelApplicationMousePress() {
    if (!applicationMousePress_.terminal) return;

    const auto terminal = applicationMousePress_.terminal;
    const int button = applicationMousePress_.button;
    const dc::terminal::SelectionPoint cell = applicationMousePress_.cell;
    applicationMousePress_ = {};
    terminal->SendMouseButtonAt(cell.row, cell.col, button, false,
                                CurrentModifiers());
    if (GetCapture() == hwnd_) ReleaseCapture();
}

void Panel::CancelPointerInteraction() {
    CancelApplicationMousePress();
    if (!selecting_) return;
    selecting_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
}

void Panel::BeginSelection(POINT point) {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    CancelApplicationMousePress();

    const dc::terminal::SelectionPoint cell = CellPointFromClient(point);
    tab->selection.anchor = cell;
    tab->selection.focus = cell;
    tab->selection.active = false;
    selecting_ = true;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Panel::UpdateSelection(POINT point) {
    if (!selecting_) return;
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    const dc::terminal::SelectionPoint cell = CellPointFromClient(point);
    if (tab->selection.focus.row == cell.row &&
        tab->selection.focus.col == cell.col) {
        return;
    }
    tab->selection.focus = cell;
    tab->selection.active =
        tab->selection.anchor.row != tab->selection.focus.row ||
        tab->selection.anchor.col != tab->selection.focus.col;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Panel::IsTerminalClientPoint(POINT point) const {
    if (!hwnd_ || point.y < tabBarHeight_) return false;
    RECT client{};
    if (!GetClientRect(hwnd_, &client)) return false;
    return point.x >= client.left && point.x < client.right &&
           point.y >= client.top && point.y < client.bottom;
}

dc::terminal::SelectionPoint Panel::CellPointForTerminal(
    POINT point, const dc::terminal::Terminal& terminal) const {
    const int rows = std::max(1, terminal.Rows());
    const int cols = std::max(1, terminal.Cols());
    return renderer_.CellAtPoint(point, tabBarHeight_, rows, cols);
}

dc::terminal::SelectionPoint Panel::CellPointFromClient(POINT point) const {
    const TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return {};
    return CellPointForTerminal(point, *tab->terminal);
}

bool Panel::CopySelection() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal || !tab->selection.active) return false;

    const std::wstring text = tab->terminal->CopySelection(tab->selection);
    if (text.empty()) return false;
    const bool copied = SetClipboardText(hwnd_, text);
    if (!copied) MessageBeep(MB_ICONWARNING);
    return copied;
}

bool Panel::PasteClipboard() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal || tab->state.load() != TabState::Running) {
        return false;
    }

    const std::wstring text = GetClipboardText(hwnd_);
    if (text.empty()) return false;

    tab->selection.active = false;
    if (tab->terminal->ScrollOffset() > 0) tab->terminal->ResetScroll();
    tab->terminal->SendPaste(text);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void Panel::SelectAll() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    const int rows = tab->terminal->Rows();
    const int cols = tab->terminal->Cols();
    if (rows <= 0 || cols <= 0) return;
    tab->selection.anchor = {0, 0};
    tab->selection.focus = {rows - 1, cols - 1};
    tab->selection.active = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}
