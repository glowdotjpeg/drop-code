#include "panel.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace dc::panel {
namespace {

void PostPanelMessage(const std::weak_ptr<LifetimeToken>& weakLifetime,
                      UINT message, WPARAM wParam, LPARAM lParam) {
    const std::shared_ptr<LifetimeToken> lifetime = weakLifetime.lock();
    if (!lifetime || !lifetime->alive.load()) return;
    const HWND hwnd = lifetime->hwnd.load();
    if (hwnd) PostMessageW(hwnd, message, wParam, lParam);
}

void PostTerminalInvalidation(
    const std::weak_ptr<LifetimeToken>& weakLifetime,
    const std::shared_ptr<UpdateGate>& updateGate, uint64_t tabId,
    uint64_t generation, UINT message) {
    const std::shared_ptr<LifetimeToken> lifetime = weakLifetime.lock();
    if (!lifetime || !lifetime->alive.load()) return;

    bool expected = false;
    if (!updateGate->updateScheduled.compare_exchange_strong(expected, true)) {
        return;
    }

    const HWND hwnd = lifetime->hwnd.load();
    if (!hwnd ||
        !PostMessageW(hwnd, message, static_cast<WPARAM>(tabId),
                      static_cast<LPARAM>(generation))) {
        updateGate->updateScheduled.store(false);
    }
}

}

void Panel::TrackWorker(std::future<void> worker) {
    ReapWorkers(false);
    workers_.push_back(std::move(worker));
}

void Panel::ReapWorkers(bool wait) {
    using namespace std::chrono_literals;
    for (auto worker = workers_.begin(); worker != workers_.end();) {
        if (!wait && worker->wait_for(0s) != std::future_status::ready) {
            ++worker;
            continue;
        }
        try {
            worker->get();
        } catch (...) {
        }
        worker = workers_.erase(worker);
    }
}

void Panel::StopTerminalAsync(
    const std::shared_ptr<dc::terminal::Terminal>& terminal) {
    if (!terminal) return;
    try {
        TrackWorker(std::async(std::launch::async,
                               [terminal] { terminal->Stop(); }));
    } catch (...) {
        terminal->Stop();
    }
}

void Panel::StartTabAsync(const std::shared_ptr<TabSession>& tab,
                          int cols, int rows) {
    if (!tab || !tab->terminal || !hwnd_) return;

    const auto token = std::make_shared<StartToken>();
    const auto updateGate = std::make_shared<UpdateGate>();
    uint64_t generation = 0;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        tab->startToken = token;
        tab->updateGate = updateGate;
        generation = tab->generation.fetch_add(1) + 1;
        tab->state.store(TabState::Starting);
    }

    const auto terminal = tab->terminal;
    const uint64_t tabId = tab->id;
    const std::wstring command = launchCommand_;
    const std::wstring directory = tab->workingDirectory;
    const std::weak_ptr<LifetimeToken> weakLifetime = lifetime_;
    terminal->SetInvalidateCallback(
        [weakLifetime, updateGate, tabId, generation] {
            PostTerminalInvalidation(weakLifetime, updateGate, tabId,
                                     generation, kTerminalUpdateMessage);
        });
    terminal->SetExitCallback([weakLifetime, tabId, generation] {
        PostPanelMessage(weakLifetime, kTabExitedMessage,
                         static_cast<WPARAM>(tabId),
                         static_cast<LPARAM>(generation));
    });

    try {
        TrackWorker(std::async(
            std::launch::async,
            [tab, token, terminal, command, directory, cols, rows,
             tabId, generation, weakLifetime] {
                bool started = false;
                try {
                    started = terminal->Start(command, directory, cols, rows,
                                              tabId);
                } catch (...) {
                    terminal->Stop();
                }

                bool stale = false;
                {
                    std::lock_guard lock(tab->lifecycleMutex);
                    stale = token->cancelled ||
                            tab->generation.load() != generation ||
                            tab->state.load() != TabState::Starting;
                    if (!stale) {
                        tab->state.store(started ? TabState::Running
                                                 : TabState::Failed);
                    }
                }

                if (stale) {
                    terminal->Stop();
                    return;
                }

                PostPanelMessage(
                    weakLifetime,
                    started ? kTabStartedMessage : kTabFailedMessage,
                    static_cast<WPARAM>(tabId),
                    static_cast<LPARAM>(generation));
        }));
    } catch (...) {
        {
            std::lock_guard lock(tab->lifecycleMutex);
            if (tab->generation.load() == generation &&
                tab->startToken == token) {
                token->cancelled = true;
                tab->state.store(TabState::Failed);
                InvalidateTabBar();
                InvalidateRect(hwnd_, nullptr, FALSE);
                MessageBeep(MB_ICONERROR);
            }
        }
        terminal->Stop();
    }
}

void Panel::StopTabAsync(const std::shared_ptr<TabSession>& tab) {
    if (!tab) return;

    std::shared_ptr<dc::terminal::Terminal> terminal;
    TabState state = TabState::Stopping;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        if (tab->startToken) tab->startToken->cancelled = true;
        state = tab->state.exchange(TabState::Stopping);
        terminal = tab->terminal;
    }

    if (state == TabState::Running || state == TabState::Failed) {
        StopTerminalAsync(terminal);
    }
}

Panel::TabSession* Panel::ActiveTab() {
    if (activeTab_ >= tabs_.size()) return nullptr;
    return tabs_[activeTab_].get();
}

const Panel::TabSession* Panel::ActiveTab() const {
    if (activeTab_ >= tabs_.size()) return nullptr;
    return tabs_[activeTab_].get();
}

void Panel::NewTab() {
    if (!hwnd_) return;

    CancelPointerInteraction();
    if (TabSession* current = ActiveTab()) current->wheelDeltaRemainder = 0;

    const bool panelFocused = GetFocus() == hwnd_;
    if (panelFocused) {
        if (TabSession* current = ActiveTab();
            current && current->terminal &&
            current->state.load() == TabState::Running) {
            current->terminal->SendFocus(false);
        }
    }

    auto tab = std::make_shared<TabSession>();
    tab->id = nextTabId_++;
    tab->workingDirectory = workingDirectory_;
    tab->terminal = std::make_shared<dc::terminal::Terminal>();

    tabs_.push_back(tab);
    activeTab_ = tabs_.size() - 1;
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    StartTabAsync(tab, renderer_.ColsForWidth(client.right - client.left),
                  renderer_.RowsForHeight(terminalHeight));
}

void Panel::CloseActiveTab() {
    if (tabs_.size() <= 1) {
        MessageBeep(MB_ICONINFORMATION);
        return;
    }

    CancelPointerInteraction();

    const bool panelFocused = GetFocus() == hwnd_;
    const auto tab = tabs_[activeTab_];
    tab->wheelDeltaRemainder = 0;
    if (panelFocused && tab->terminal &&
        tab->state.load() == TabState::Running) {
        tab->terminal->SendFocus(false);
    }
    StopTabAsync(tab);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(activeTab_));
    if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.size() - 1;
    tabs_[activeTab_]->wheelDeltaRemainder = 0;
    if (tabs_[activeTab_]->updateGate) {
        tabs_[activeTab_]->updateGate->updateScheduled.store(false);
    }
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);
    if (panelFocused) {
        if (TabSession* current = ActiveTab();
            current && current->terminal &&
            current->state.load() == TabState::Running) {
            current->terminal->SendFocus(true);
        }
    }
}

void Panel::SelectTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    const bool panelFocused = GetFocus() == hwnd_;
    const bool changed = index != static_cast<int>(activeTab_);
    if (changed) {
        CancelPointerInteraction();
        if (TabSession* current = ActiveTab()) {
            current->wheelDeltaRemainder = 0;
        }
        if (panelFocused) {
            if (TabSession* current = ActiveTab();
                current && current->terminal &&
                current->state.load() == TabState::Running) {
                current->terminal->SendFocus(false);
            }
        }
    }
    activeTab_ = static_cast<size_t>(index);
    if (TabSession* current = ActiveTab()) {
        current->wheelDeltaRemainder = 0;
        if (current->updateGate) {
            current->updateGate->updateScheduled.store(false);
        }
    }
    ResizeTerminal();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
    SetFocus(hwnd_);
    if (panelFocused && changed) {
        if (TabSession* current = ActiveTab();
            current && current->terminal &&
            current->state.load() == TabState::Running) {
            current->terminal->SendFocus(true);
        }
    }
}

void Panel::SelectRelativeTab(int direction) {
    if (tabs_.size() <= 1) return;
    const int count = static_cast<int>(tabs_.size());
    int next = static_cast<int>(activeTab_) + direction;
    if (next < 0) next = count - 1;
    if (next >= count) next = 0;
    SelectTab(next);
}

void Panel::RestartActiveTab() {
    TabSession* tab = ActiveTab();
    if (!tab || !tab->terminal) return;

    CancelPointerInteraction();
    tab->wheelDeltaRemainder = 0;

    if (GetFocus() == hwnd_ && tab->state.load() == TabState::Running) {
        tab->terminal->SendFocus(false);
    }

    std::shared_ptr<dc::terminal::Terminal> oldTerminal;
    TabState oldState = TabState::Stopping;
    {
        std::lock_guard lock(tab->lifecycleMutex);
        if (tab->startToken) tab->startToken->cancelled = true;
        oldState = tab->state.load();
        oldTerminal = tab->terminal;
        tab->terminal = std::make_shared<dc::terminal::Terminal>();
        tab->state.store(TabState::Starting);
    }
    if (oldState == TabState::Running || oldState == TabState::Failed) {
        StopTerminalAsync(oldTerminal);
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    const int terminalHeight = std::max(
        1L, static_cast<long>(client.bottom - client.top - tabBarHeight_));
    const int rows = renderer_.RowsForHeight(terminalHeight);
    const int cols = renderer_.ColsForWidth(client.right - client.left);
    StartTabAsync(tabs_[activeTab_], cols, rows);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateTabBar();
}

Panel::TabSession* Panel::FindTab(uint64_t tabId) {
    for (const auto& tab : tabs_) {
        if (tab && tab->id == tabId) return tab.get();
    }
    return nullptr;
}

}
