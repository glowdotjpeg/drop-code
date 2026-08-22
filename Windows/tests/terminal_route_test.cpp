#include "terminal.h"

#include <cstddef>
#include <iostream>
#include <string>
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

    static void SetRoutingState(Terminal& terminal, int mouseMode,
                                bool alternateScreen, int scrollbackLines) {
        std::lock_guard lock(terminal.mutex_);
        terminal.mouseMode_ = mouseMode;
        terminal.alternateScreen_ = alternateScreen;
        terminal.scrollOffset_ = 0;
        terminal.scrollback_.clear();
        for (int i = 0; i < scrollbackLines; ++i) {
            terminal.scrollback_.push_back({});
        }
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

    constexpr char enableSgrMouse[] = "\x1b[?1003h\x1b[?1006h";
    passed &= Expect(TerminalRouteTestAccessor::Feed(terminal, enableSgrMouse),
                     "libvterm did not consume SGR mouse setup");
    TerminalRouteTestAccessor::DrainOutput(terminal);
    TerminalRouteTestAccessor::SetRoutingState(
        terminal, VTERM_PROP_MOUSE_MOVE, false, 3);

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
                     "application wheel changed local scrollback");

    const Terminal::WheelRoute downRoute =
        terminal.RouteWheel(7, 12, -2, VTERM_MOD_NONE);
    const std::string downOutput =
        TerminalRouteTestAccessor::DrainOutput(terminal);
    passed &= Expect(downRoute == Terminal::WheelRoute::Application,
                     "two-notch wheel-down was not application-routed");
    passed &= Expect(downOutput ==
                         "\x1b[<65;13;8M\x1b[<65;13;8M",
                     "wheel-down output was not exactly two stable SGR packets");
    return passed;
}

bool TestMouseOffRoutes() {
    Terminal terminal;
    bool passed = Expect(TerminalRouteTestAccessor::Initialize(terminal),
                         "failed to initialize mouse-off test state");
    if (!passed) return false;

    TerminalRouteTestAccessor::SetRoutingState(
        terminal, VTERM_PROP_MOUSE_NONE, true, 3);
    const Terminal::WheelRoute alternateRoute =
        terminal.RouteWheel(7, 12, 1, VTERM_MOD_NONE);
    passed &= Expect(alternateRoute == Terminal::WheelRoute::Ignored,
                     "alternate-screen mouse-off wheel was not ignored");
    passed &= Expect(terminal.ScrollOffset() == 0,
                     "alternate-screen mouse-off wheel changed local scrollback");
    passed &= Expect(TerminalRouteTestAccessor::DrainOutput(terminal).empty(),
                     "alternate-screen mouse-off wheel emitted terminal output");

    TerminalRouteTestAccessor::SetRoutingState(
        terminal, VTERM_PROP_MOUSE_NONE, false, 3);
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

bool TestScrollbackPopInitialization() {
    Terminal terminal;
    return Expect(
        TerminalRouteTestAccessor::PopLineInitializesBlankCells(terminal),
        "scrollback pop left resized cells uninitialized");
}

}  // namespace

int main() {
    const bool passed = TestApplicationWheel() && TestMouseOffRoutes() &&
                        TestScrollbackPopInitialization();
    if (passed) std::cout << "PASS: production Terminal::RouteWheel\n";
    return passed ? 0 : 1;
}
