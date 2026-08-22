#pragma once
#include <Windows.h>

#ifdef small
#undef small
#endif

#include <vterm.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

constexpr UINT kAnimationDurationMs = 140;
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

    void ToggleLatched();
    void RestartTerminal();
    void SetHeightPercentage(int percent);
    void SetOpacityPercentage(int percent);
    bool ApplySessionSettings(const std::wstring& command,
                              const std::wstring& directory);

    int HeightPercentage() const { return heightPercent_; }
    int OpacityPercentage() const { return opacityPercent_; }
    const std::wstring& LaunchCommand() const { return launchCommand_; }
    const std::wstring& WorkingDirectory() const { return workingDirectory_; }

    void OnTapToggle() override;
    bool OnHoldStart() override;
    void OnHoldEnd(bool opened) override;
    void OnCancel() override;

private:
    static constexpr wchar_t kWindowClass[] = L"DropCodePanelWindow";
    static constexpr wchar_t kTabBarClass[] = L"DropCodeTabBarWindow";
    static constexpr UINT kTerminalUpdateMessage = WM_APP + 1;
    static constexpr UINT kTabStartedMessage = WM_APP + 3;
    static constexpr UINT kTabFailedMessage = WM_APP + 4;
    static constexpr UINT kTabExitedMessage = WM_APP + 5;
    static constexpr UINT kChordInputReleasedMessage = WM_APP + 6;
    static constexpr UINT kAnimationFrameMessage = WM_APP + 7;
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TabBarWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam);
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam,
                                              LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleTabBarMessage(HWND tabBar, UINT msg, WPARAM wParam,
                                LPARAM lParam);

    void SetVisible(bool visible);
    void BeginAnimation(int toY, bool entering);
    void AnimationTick();
    void StartAnimationClock();
    void StopAnimationClock();
    void ApplyOpacity();
    void ResizeTerminal();
    void ResizeTabBar();
    void RestorePreviousForeground();
    void PaintTabBar(HDC dc);
    void HandleTabBarClick(int x, int y);
    void InvalidateTabBar();
    void StartMouseWheelHook();
    void StopMouseWheelHook();

    enum class TabState : uint8_t {
        Starting,
        Running,
        Failed,
        Stopping,
    };

    struct StartToken {
        bool cancelled = false;
    };

    struct TabSession {
        uint64_t id = 0;
        std::wstring workingDirectory;
        std::shared_ptr<dc::terminal::Terminal> terminal;
        std::shared_ptr<StartToken> startToken;
        std::shared_ptr<UpdateGate> updateGate;
        std::mutex lifecycleMutex;
        dc::terminal::SelectionRange selection;
        int wheelDeltaRemainder = 0;
        std::atomic<TabState> state{TabState::Starting};
        std::atomic<uint64_t> generation{0};
    };

    struct ApplicationMousePress {
        int button = 0;
        dc::terminal::SelectionPoint cell;
        std::shared_ptr<dc::terminal::Terminal> terminal;
    };

    bool CreateTabBar();
    void StartTabAsync(const std::shared_ptr<TabSession>& tab,
                       int cols, int rows);
    void StopTabAsync(const std::shared_ptr<TabSession>& tab);
    void StopTerminalAsync(
        const std::shared_ptr<dc::terminal::Terminal>& terminal);
    void TrackWorker(std::future<void> worker);
    void ReapWorkers(bool wait);
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

    void HandleKeyDown(UINT vk);
    void HandleChar(wchar_t ch);
    void SuppressTranslatedChar(wchar_t expected);
    bool HandleMouseWheel(short delta, POINT screenPoint,
                          VTermModifier modifiers, bool nonBlocking);
    bool BeginApplicationMousePress(int button, POINT point,
                                    VTermModifier modifiers);
    bool EndApplicationMousePress(int button, POINT point);
    void UpdateApplicationMousePress(POINT point);
    void CancelApplicationMousePress();
    void CancelPointerInteraction();
    void BeginSelection(POINT point);
    void UpdateSelection(POINT point);
    bool IsTerminalClientPoint(POINT point) const;
    dc::terminal::SelectionPoint CellPointForTerminal(
        POINT point, const dc::terminal::Terminal& terminal) const;
    dc::terminal::SelectionPoint CellPointFromClient(POINT point) const;
    bool CopySelection();
    bool PasteClipboard();
    void SelectAll();
    VTermModifier CurrentModifiers() const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND previousForeground_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    static Panel* mouseHookOwner_;
    std::shared_ptr<LifetimeToken> lifetime_;

    renderer::TermRenderer renderer_;
    hotkey::ChordMonitor chordMonitor_;
    HWND tabBar_ = nullptr;
    std::vector<std::shared_ptr<TabSession>> tabs_;
    std::vector<std::future<void>> workers_;
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
    std::jthread animationClock_;
    std::atomic<bool> animationFramePending_{false};

    bool selecting_ = false;
    ApplicationMousePress applicationMousePress_;
    bool chordInputSuppressed_ = false;
    bool translatedCharPending_ = false;
    wchar_t expectedTranslatedChar_ = 0;
    LONG translatedCharMessageTime_ = 0;
    wchar_t pendingHighSurrogate_ = 0;
};

}
