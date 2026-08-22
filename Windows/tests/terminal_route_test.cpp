#include "terminal.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace dc::terminal {

struct TerminalRouteTestAccessor {
    static bool Initialize(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        terminal.vt_ = vterm_new(24, 80);
        if (!terminal.vt_) return false;

        vterm_set_utf8(terminal.vt_, 1);
        terminal.screen_ = vterm_obtain_screen(terminal.vt_);
        VTermState* state = vterm_obtain_state(terminal.vt_);
        if (!terminal.screen_ || !state) return false;

        static const VTermScreenCallbacks callbacks = [] {
            VTermScreenCallbacks value{};
            value.settermprop = Terminal::OnSetTermProp;
            return value;
        }();
        vterm_screen_set_callbacks(terminal.screen_, &callbacks, &terminal);
        vterm_screen_enable_altscreen(terminal.screen_, 1);
        vterm_state_reset(state, 1);
        return true;
    }

    static bool Feed(Terminal& terminal, const std::string& input) {
        std::lock_guard lock(terminal.mutex_);
        return vterm_input_write(terminal.vt_, input.data(), input.size()) ==
               input.size();
    }

    static std::string DrainOutput(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        std::string output;
        char buffer[256];
        while (vterm_output_get_buffer_current(terminal.vt_) != 0) {
            const size_t bytesRead =
                vterm_output_read(terminal.vt_, buffer, sizeof(buffer));
            if (bytesRead == 0) break;
            output.append(buffer, bytesRead);
        }
        return output;
    }

    static void SetScrollbackLines(Terminal& terminal, int scrollbackLines,
                                   int scrollOffset = 0) {
        std::lock_guard lock(terminal.mutex_);
        terminal.scrollback_.clear();
        for (int i = 0; i < scrollbackLines; ++i) {
            terminal.scrollback_.push_back({});
        }
        terminal.scrollOffset_ = scrollOffset;
    }

    static size_t ScrollbackSize(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        return terminal.scrollback_.size();
    }

    static int MouseMode(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        return terminal.mouseMode_;
    }

    static bool AlternateScreen(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        return terminal.alternateScreen_;
    }

    static bool PopLineInitializesBlankCells(Terminal& terminal) {
        std::lock_guard lock(terminal.mutex_);
        ScrollbackLine line;
        line.cells.resize(1);
        line.cells[0].chars[0] = L'X';
        line.cells[0].width = 1;
        terminal.scrollback_.push_back(std::move(line));

        VTermScreenCell cells[4]{};
        if (Terminal::OnSbPopLine(4, cells, &terminal) != 1) return false;
        if (cells[0].chars[0] != L'X' || cells[0].width != 1) return false;
        for (int col = 1; col < 4; ++col) {
            if (cells[col].chars[0] != 0 || cells[col].width != 1 ||
                !VTERM_COLOR_IS_DEFAULT_FG(&cells[col].fg) ||
                !VTERM_COLOR_IS_DEFAULT_BG(&cells[col].bg)) {
                return false;
            }
        }
        return terminal.scrollback_.empty();
    }
};

}  // namespace dc::terminal

namespace {

using dc::terminal::Terminal;
using dc::terminal::TerminalRouteTestAccessor;

bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAIL: " << message << '\n';
    return false;
}

bool TestApplicationWheel() {
    Terminal terminal;
    bool passed = Expect(TerminalRouteTestAccessor::Initialize(terminal),
                         "failed to initialize Terminal test state");
    if (!passed) return false;

    constexpr char enableSgrMouse[] =
        "\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h";
    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, enableSgrMouse),
                     "libvterm did not consume SGR mouse setup");
    TerminalRouteTestAccessor::DrainOutput(terminal);
    TerminalRouteTestAccessor::SetScrollbackLines(terminal, 3, 2);
    passed &= Expect(
        TerminalRouteTestAccessor::MouseMode(terminal) ==
            VTERM_PROP_MOUSE_MOVE,
        "OpenTUI mouse setup did not enable application mouse routing");

    const Terminal::WheelRoute upRoute =
        terminal.RouteWheel(7, 12, 2, VTERM_MOD_NONE);
    const std::string upOutput =
        TerminalRouteTestAccessor::DrainOutput(terminal);
    passed &= Expect(upRoute == Terminal::WheelRoute::Application,
                     "two-notch wheel-up was not application-routed");
    passed &= Expect(upOutput ==
                         "\x1b[<64;13;8M\x1b[<64;13;8M",
                     "wheel-up output was not exactly two stable SGR packets");
    passed &= Expect(terminal.ScrollOffset() == 0,
                     "application wheel did not leave host scrollback mode");
    passed &= Expect(TerminalRouteTestAccessor::ScrollbackSize(terminal) == 3,
                     "application wheel changed stored scrollback");

    const Terminal::WheelRoute downRoute =
        terminal.RouteWheel(7, 12, -2, VTERM_MOD_NONE);
    const std::string downOutput =
        TerminalRouteTestAccessor::DrainOutput(terminal);
    passed &= Expect(downRoute == Terminal::WheelRoute::Application,
                     "two-notch wheel-down was not application-routed");
    passed &= Expect(downOutput ==
                         "\x1b[<65;13;8M\x1b[<65;13;8M",
                     "wheel-down output was not exactly two stable SGR packets");

    constexpr char disableMovement[] =
        "\x1b[?1003l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
    passed &= Expect(
        TerminalRouteTestAccessor::Feed(terminal, disableMovement),
        "libvterm did not consume OpenTUI's movement-disabled sequence");
    passed &= Expect(
        TerminalRouteTestAccessor::MouseMode(terminal) ==
            VTERM_PROP_MOUSE_DRAG,
        "OpenTUI's movement-disabled sequence did not select drag mode");
    const Terminal::WheelRoute remainingMouseRoute =
        terminal.RouteWheel(7, 12, 1, VTERM_MOD_NONE);
    passed &= Expect(remainingMouseRoute == Terminal::WheelRoute::Application,
                     "remaining button-event mode did not capture the wheel");
    passed &= Expect(TerminalRouteTestAccessor::DrainOutput(terminal) ==
                         "\x1b[<64;13;8M",
                     "button-event mode emitted the wrong wheel packet");

    constexpr char teardown[] =
        "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l";
    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, teardown),
                     "libvterm did not consume OpenTUI's mouse teardown");
    passed &= Expect(
        TerminalRouteTestAccessor::MouseMode(terminal) ==
            VTERM_PROP_MOUSE_NONE,
        "OpenTUI's mouse teardown left application routing enabled");
    return passed;
}

bool TestMouseOffRoutes() {
    Terminal terminal;
    bool passed = Expect(TerminalRouteTestAccessor::Initialize(terminal),
                         "failed to initialize mouse-off test state");
    if (!passed) return false;

    constexpr char enterAlternateScreen[] = "\x1b[?1049h";
    TerminalRouteTestAccessor::SetScrollbackLines(terminal, 3);
    passed &= Expect(
        TerminalRouteTestAccessor::Feed(terminal, enterAlternateScreen),
        "libvterm did not enter the alternate screen");
    const Terminal::WheelRoute alternateRoute =
        terminal.RouteWheel(7, 12, 1, VTERM_MOD_NONE);
    passed &= Expect(alternateRoute == Terminal::WheelRoute::Application,
                     "alternate-screen wheel was not application-routed");
    passed &= Expect(terminal.ScrollOffset() == 0,
                     "alternate-screen mouse-off wheel changed local scrollback");
    passed &= Expect(TerminalRouteTestAccessor::DrainOutput(terminal) ==
                         "\x1b[A\x1b[A\x1b[A",
                     "alternate-screen wheel did not emit three cursor-up keys");

    constexpr char disableAlternateScroll[] = "\x1b[?1007l";
    passed &= Expect(
        TerminalRouteTestAccessor::Feed(terminal, disableAlternateScroll),
        "libvterm did not disable alternate scrolling");
    const Terminal::WheelRoute disabledRoute =
        terminal.RouteWheel(7, 12, 1, VTERM_MOD_NONE);
    passed &= Expect(disabledRoute == Terminal::WheelRoute::Ignored,
                     "disabled alternate scrolling still emitted input");
    passed &= Expect(terminal.ScrollOffset() == 0,
                     "disabled alternate scrolling used host scrollback");
    passed &= Expect(TerminalRouteTestAccessor::DrainOutput(terminal).empty(),
                     "disabled alternate scrolling emitted terminal output");

    constexpr char leaveAlternateScreen[] = "\x1b[?1049l";
    passed &= Expect(
        TerminalRouteTestAccessor::Feed(terminal, leaveAlternateScreen),
        "libvterm did not leave the alternate screen");
    TerminalRouteTestAccessor::SetScrollbackLines(terminal, 3);
    const Terminal::WheelRoute primaryRoute =
        terminal.RouteWheel(7, 12, 2, VTERM_MOD_NONE);
    passed &= Expect(primaryRoute == Terminal::WheelRoute::Scrollback,
                     "primary-screen mouse-off wheel was not locally routed");
    passed &= Expect(terminal.ScrollOffset() == 2,
                     "primary-screen local scrollback moved by the wrong amount");
    passed &= Expect(TerminalRouteTestAccessor::DrainOutput(terminal).empty(),
                     "primary-screen local scrollback emitted terminal output");
    return passed;
}

bool TestTerminalResets() {
    Terminal terminal;
    bool passed = Expect(TerminalRouteTestAccessor::Initialize(terminal),
                         "failed to initialize terminal-reset test state");
    if (!passed) return false;

    constexpr char enableMouse[] =
        "\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h";
    constexpr char softReset[] = "\x1b[!p";
    constexpr char hardReset[] = "\x1b" "c";

    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, enableMouse),
                     "libvterm did not consume setup before soft reset");
    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, softReset),
                     "libvterm did not consume DECSTR");
    passed &= Expect(
        TerminalRouteTestAccessor::MouseMode(terminal) ==
            VTERM_PROP_MOUSE_NONE,
        "DECSTR left application mouse routing enabled");
    passed &= Expect(!TerminalRouteTestAccessor::AlternateScreen(terminal),
                     "DECSTR left alternate-screen routing enabled");

    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, enableMouse),
                     "libvterm did not consume setup before hard reset");
    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, hardReset),
                     "libvterm did not consume RIS");
    passed &= Expect(
        TerminalRouteTestAccessor::MouseMode(terminal) ==
            VTERM_PROP_MOUSE_NONE,
        "RIS left application mouse routing enabled");
    passed &= Expect(!TerminalRouteTestAccessor::AlternateScreen(terminal),
                     "RIS left alternate-screen routing enabled");

    TerminalRouteTestAccessor::SetScrollbackLines(terminal, 2);
    passed &= Expect(
        terminal.RouteWheel(7, 12, 1, VTERM_MOD_NONE) ==
            Terminal::WheelRoute::Scrollback,
        "wheel was black-holed after terminal reset");
    return passed;
}

bool TestNonBlockingWheel() {
    Terminal terminal;
    bool passed = Expect(TerminalRouteTestAccessor::Initialize(terminal),
                         "failed to initialize nonblocking-wheel test state");
    if (!passed) return false;

    std::optional<Terminal::WheelRoute> route;
    {
        std::unique_lock terminalLock(terminal.Lock());
        std::thread attempt([&] {
            route = terminal.TryRouteWheel(7, 12, 1, VTERM_MOD_NONE);
        });
        attempt.join();
    }
    return Expect(!route,
                  "nonblocking wheel waited for a contended terminal lock");
}

bool TestScrollbackPopInitialization() {
    Terminal terminal;
    return Expect(
        TerminalRouteTestAccessor::PopLineInitializesBlankCells(terminal),
        "scrollback pop left resized cells uninitialized");
}

}  // namespace

int main() {
    const bool passed = TestApplicationWheel() && TestMouseOffRoutes() &&
                        TestTerminalResets() &&
                        TestNonBlockingWheel() &&
                        TestScrollbackPopInitialization();
    if (passed) std::cout << "PASS: production Terminal::RouteWheel\n";
    return passed ? 0 : 1;
}
