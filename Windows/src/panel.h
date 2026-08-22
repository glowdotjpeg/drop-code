#pragma once
#include <Windows.h>

#ifdef small
#undef small
#endif

#include <vterm.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hotkey.h"
#include "terminal.h"
#include "vtrenderer.h"

namespace dc::panel {

struct LifetimeToken {
    std::atomic<bool> alive{true};
    std::atomic<HWND> hwnd{nullptr};
};

struct UpdateGate {
    std::atomic<bool> updateScheduled{false};
};

constexpr UINT kAnimationTimerId = 1;
constexpr UINT kTerminalFrameTimerId = 3;
constexpr UINT kAnimationDurationMs = 140;
constexpr UINT kAnimationTimerIntervalMs = 8;
constexpr UINT kMinHeightPercent = 20;
constexpr UINT kMaxHeightPercent = 100;

class Panel : public hotkey::ChordMonitor::Listener {
public:
    Panel();
    ~Panel();

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    bool Create(HINSTANCE instance);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    void ToggleLatched();
    bool BeginMomentary();
    void EndMomentary(bool openedByGesture);
    void RestartTerminal();
    void SetHeightPercentage(int percent);
    void SetOpacityPercentage(int percent);
    bool ApplySessionSettings(const std::wstring& command,
                              const std::wstring& directory);

    int HeightPercentage() const { return heightPercent_; }
    int OpacityPercentage() const { return opacityPercent_; }
    const std::wstring& LaunchCommand() const { return launchCommand_; }
    const std::wstring& WorkingDirectory() const { return workingDirectory_; }
    bool IsVisible() const { return isVisible_; }

    void OnTapToggle() override;
    bool OnHoldStart() override;
    void OnHoldEnd(bool opened) override;
    void OnCancel() override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TabBarWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleTabBarMessage(HWND tabBar, UINT msg, WPARAM wParam,
                                LPARAM lParam);

    void SetVisible(bool visible);
    void BeginAnimation(int toY, bool entering);
    void AnimationTick();
    void ReleaseAnimationTimerResolution();
    void ApplyOpacity();
    void ResizeTerminal();
    void ResizeTabBar();
    void RestorePreviousForeground();
    void PaintTabBar(HDC dc);
    void HandleTabBarClick(int x, int y);
    void InvalidateTabBar();

    enum class TabState : uint8_t {
        Starting,
        Running,
        Failed,
        Stopping,
    };

    struct StartToken {
        std::atomic<bool> cancelled{false};
    };

    struct TabSession {
        uint64_t id = 0;
        std::wstring workingDirectory;
        std::shared_ptr<dc::terminal::Terminal> terminal;
        std::shared_ptr<StartToken> startToken;
        std::mutex lifecycleMutex;
        dc::terminal::SelectionRange selection;
        std::atomic<TabState> state{TabState::Starting};
        std::atomic<uint64_t> generation{0};
    };

    bool CreateTabBar();
    void StartTabAsync(const std::shared_ptr<TabSession>& tab,
                       int cols, int rows);
    void StopTabAsync(const std::shared_ptr<TabSession>& tab);
    void NewTab();
    void CloseActiveTab();
    void SelectTab(int index);
    void SelectRelativeTab(int direction);
    void RestartActiveTab();
    TabSession* ActiveTab();
    const TabSession* ActiveTab() const;
    int TabBarHeightForWindow() const;
    int TabWidth() const;
    RECT TabRect(int index) const;
    RECT PlusButtonRect() const;
    std::wstring TabLabel(int index) const;
    TabSession* FindTab(uint64_t tabId);

    void HandleKeyDown(UINT vk, bool sysKey);
    void HandleChar(wchar_t ch);
    void HandleMouseWheel(short delta, LPARAM lParam);
    void BeginSelection(POINT point);
    void UpdateSelection(POINT point);
    dc::terminal::SelectionPoint CellPointFromClient(POINT point) const;
    bool CopySelection();
    bool PasteClipboard();
    void SelectAll();
    VTermModifier CurrentModifiers() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND previousForeground_ = nullptr;
    std::shared_ptr<LifetimeToken> lifetime_;
    std::shared_ptr<UpdateGate> updateGate_;

    renderer::TermRenderer renderer_;
    hotkey::ChordMonitor chordMonitor_;
    HWND tabBar_ = nullptr;
    std::vector<std::shared_ptr<TabSession>> tabs_;
    size_t activeTab_ = 0;
    uint64_t nextTabId_ = 1;

    bool isVisible_ = false;
    bool isLatched_ = false;
    int heightPercent_ = 40;
    int opacityPercent_ = 90;
    std::wstring launchCommand_ = L"opencode";
    std::wstring workingDirectory_;
    int tabBarHeight_ = 36;

    bool animating_ = false;
    bool animEntering_ = false;
    int animStartY_ = 0;
    int animEndY_ = 0;
    int animLeft_ = 0;
    LONGLONG animStartCounter_ = 0;
    bool terminalFramePending_ = false;
    bool animationTimerResolutionActive_ = false;

    bool selecting_ = false;
    bool chordInputSuppressed_ = false;
    bool suppressNextChar_ = false;
    int wheelDeltaRemainder_ = 0;
    wchar_t pendingHighSurrogate_ = 0;
};

}
