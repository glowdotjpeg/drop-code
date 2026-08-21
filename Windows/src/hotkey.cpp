#include "hotkey.h"

namespace dc::hotkey {
namespace {

bool IsChordKeyDown() {
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
           (GetAsyncKeyState('T') & 0x8000) != 0;
}

bool IsModifierOrChordKey(int vk) {
    switch (vk) {
        case VK_CONTROL:
        case VK_MENU:
        case VK_SHIFT:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LWIN:
        case VK_RWIN:
        case 'T':
            return true;
        default:
            return false;
    }
}

bool HasOtherKeyDown() {
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == 'T' || IsModifierOrChordKey(vk)) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) != 0) {
            return true;
        }
    }
    return false;
}

}

uint64_t TickCountMs() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<uint64_t>(value.QuadPart) : 1ULL;
    }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return (static_cast<uint64_t>(counter.QuadPart) * 1000ULL) / frequency;
}

bool ChordMonitor::Start(HWND hwnd, Listener& listener) {
    Stop();
    if (!hwnd) return false;
    hwnd_ = hwnd;
    listener_ = &listener;
    if (RegisterHotKey(hwnd_, kChordHotkeyId,
                       MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'T')) {
        return true;
    }
    hwnd_ = nullptr;
    listener_ = nullptr;
    return false;
}

void ChordMonitor::Stop() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, kChordHotkeyId);
        KillTimer(hwnd_, kChordPollTimerId);
    }
    hwnd_ = nullptr;
    listener_ = nullptr;
    ResetGesture();
}

void ChordMonitor::OnHotkey() {
    if (chordIsDown_) {
        CancelGesture();
        return;
    }
    chordIsDown_ = true;
    gestureIsValid_ = true;
    holdWasTriggered_ = false;
    openedForHold_ = false;
    chordDownTick_ = TickCountMs();
    SetTimer(hwnd_, kChordPollTimerId, kPollIntervalMs, nullptr);
}

void ChordMonitor::OnPollTick() {
    if (!chordIsDown_) {
        ResetGesture();
        KillTimer(hwnd_, kChordPollTimerId);
        return;
    }

    if (HasOtherKeyDown()) {
        CancelGesture();
        return;
    }

    if (!IsChordKeyDown()) {
        FinishGesture();
        return;
    }

    if (!holdWasTriggered_ && gestureIsValid_) {
        const uint64_t elapsed = TickCountMs() - chordDownTick_;
        if (elapsed >= kHoldDelayMs) {
            holdWasTriggered_ = true;
            openedForHold_ = listener_ ? listener_->OnHoldStart() : false;
        }
    }
}

void ChordMonitor::CancelGesture() {
    KillTimer(hwnd_, kChordPollTimerId);
    const bool holdWasTriggered = holdWasTriggered_;
    const bool openedForHold = openedForHold_;
    Listener* listener = listener_;
    ResetGesture();
    if (holdWasTriggered && listener) {
        listener->OnHoldEnd(openedForHold);
    } else if (listener) {
        listener->OnCancel();
    }
}

void ChordMonitor::FinishGesture() {
    KillTimer(hwnd_, kChordPollTimerId);
    chordIsDown_ = false;
    if (!gestureIsValid_) {
        ResetGesture();
        return;
    }
    if (holdWasTriggered_ && listener_) {
        listener_->OnHoldEnd(openedForHold_);
    } else if (listener_) {
        listener_->OnTapToggle();
    }
    ResetGesture();
}

void ChordMonitor::ResetGesture() {
    gestureIsValid_ = false;
    holdWasTriggered_ = false;
    openedForHold_ = false;
    chordIsDown_ = false;
}

}
