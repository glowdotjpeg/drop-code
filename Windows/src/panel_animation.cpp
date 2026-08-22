#include "panel.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace dc::panel {
namespace {

float EaseOutCubic(float t) {
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

LONGLONG AnimationCounter() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double AnimationCounterFrequency() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? static_cast<double>(value.QuadPart) : 1.0;
    }();
    return frequency;
}

bool ReduceMotionEnabled() {
    BOOL animations = FALSE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return !animations;
}

}

void Panel::BeginAnimation(int toY, bool entering) {
    StopAnimationClock();

    RECT current{};
    GetWindowRect(hwnd_, &current);
    const int fromY = current.top;
    if (ReduceMotionEnabled() || fromY == toY) {
        RECT r{};
        GetWindowRect(hwnd_, &r);
        SetWindowPos(hwnd_, HWND_TOPMOST, r.left, toY, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        if (!entering) {
            ShowWindow(hwnd_, SW_HIDE);
            RestorePreviousForeground();
        }
        animating_ = false;
        return;
    }
    animating_ = true;
    animEntering_ = entering;
    animStartY_ = fromY;
    animEndY_ = toY;
    animLeft_ = current.left;
    animStartCounter_ = AnimationCounter();
    AnimationTick();
    if (animating_) StartAnimationClock();
}

void Panel::AnimationTick() {
    if (!animating_) return;

    const LONGLONG elapsedCounter =
        std::max<LONGLONG>(0, AnimationCounter() - animStartCounter_);
    const double elapsedMs =
        (static_cast<double>(elapsedCounter) / AnimationCounterFrequency()) * 1000.0;
    const float t = std::min(
        1.0f, static_cast<float>(elapsedMs / kAnimationDurationMs));
    const float eased = EaseOutCubic(t);
    const int y = static_cast<int>(std::lround(
        animStartY_ + (animEndY_ - animStartY_) * eased));

    SetWindowPos(hwnd_, nullptr, animLeft_, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);

    if (t >= 1.0f) {
        animating_ = false;
        if (!animEntering_) {
            ShowWindow(hwnd_, SW_HIDE);
            RestorePreviousForeground();
        }
        StopAnimationClock();
    }
}

void Panel::StartAnimationClock() {
    StopAnimationClock();
    animationClock_ = std::jthread([this](std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            if (FAILED(DwmFlush())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            if (stopToken.stop_requested()) break;

            bool expected = false;
            if (!animationFramePending_.compare_exchange_strong(expected,
                                                                 true)) {
                continue;
            }
            const std::shared_ptr<LifetimeToken> lifetime = lifetime_;
            const HWND hwnd = lifetime && lifetime->alive.load()
                                  ? lifetime->hwnd.load()
                                  : nullptr;
            if (!hwnd ||
                !PostMessageW(hwnd, kAnimationFrameMessage, 0, 0)) {
                animationFramePending_.store(false);
            }
        }
    });
}

void Panel::StopAnimationClock() {
    if (animationClock_.joinable()) {
        animationClock_.request_stop();
        animationClock_.join();
    }
    animationFramePending_.store(false);
}

void Panel::ApplyOpacity() {
    if (!hwnd_) return;
    SetLayeredWindowAttributes(hwnd_, 0,
                               static_cast<BYTE>(opacityPercent_ * 255 / 100),
                               LWA_ALPHA);
}

void Panel::ResizeTerminal() {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    const int rows = renderer_.RowsForHeight(terminalHeight);
    const int cols = renderer_.ColsForWidth(client.right - client.left);
    TabSession* tab = ActiveTab();
    if (tab && tab->terminal && tab->state.load() == TabState::Running) {
        tab->terminal->Resize(cols, rows);
    }
}

void Panel::RestorePreviousForeground() {
    if (previousForeground_ && IsWindow(previousForeground_)) {
        SetForegroundWindow(previousForeground_);
    }
    previousForeground_ = nullptr;
}

}
