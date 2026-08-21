#pragma once
#include <Windows.h>

#include <cstdint>

namespace dc::hotkey {

constexpr UINT kChordHotkeyId = 1;
constexpr int kHoldDelayMs = 200;
constexpr int kPollIntervalMs = 8;

class ChordMonitor {
public:
    struct Listener {
        virtual void OnTapToggle() = 0;
        virtual bool OnHoldStart() = 0;
        virtual void OnHoldEnd(bool opened) = 0;
        virtual void OnCancel() = 0;
    };

    bool Start(HWND hwnd, Listener& listener);
    void Stop();

    void OnHotkey();
    void OnPollTick();
    bool IsChordDown() const { return chordIsDown_; }

    static constexpr UINT kChordPollTimerId = 2;

private:
    void CancelGesture();
    void FinishGesture();
    void ResetGesture();

    HWND hwnd_ = nullptr;
    Listener* listener_ = nullptr;
    bool chordIsDown_ = false;
    bool gestureIsValid_ = false;
    bool holdWasTriggered_ = false;
    bool openedForHold_ = false;
    uint64_t chordDownTick_ = 0;
};

uint64_t TickCountMs();

}
