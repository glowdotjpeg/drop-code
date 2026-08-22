#include <vterm.h>

#include <cstddef>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string DrainOutput(VTerm* terminal) {
    std::string output;
    char buffer[256];

    while (vterm_output_get_buffer_current(terminal) != 0) {
        const std::size_t bytes_read =
            vterm_output_read(terminal, buffer, sizeof(buffer));
        if (bytes_read == 0) {
            break;
        }
        output.append(buffer, bytes_read);
    }

    return output;
}

std::string DescribeBytes(const std::string& value) {
    std::ostringstream description;
    description << '"';

    for (const unsigned char byte : value) {
        if (byte == 0x1b) {
            description << "\\x1B";
        } else if (byte >= 0x20 && byte <= 0x7e) {
            description << static_cast<char>(byte);
        } else {
            description << "\\x" << std::uppercase << std::hex
                        << std::setw(2) << std::setfill('0')
                        << static_cast<unsigned int>(byte) << std::dec;
        }
    }

    description << "\" (" << value.size() << " bytes)";
    return description.str();
}

bool ExpectWheelPacket(VTerm* terminal, int button, VTermModifier modifiers,
                       const char* expected, const char* caseName) {
    vterm_mouse_button_at(
        terminal, 7, 12, button, true, modifiers);

    const std::string actual = DrainOutput(terminal);
    if (actual == expected) {
        std::cout << "PASS: " << caseName << '\n';
        return true;
    }

    std::cerr << "FAIL: " << caseName << '\n'
              << "  expected exactly: " << DescribeBytes(expected) << '\n'
              << "  actual:           " << DescribeBytes(actual) << '\n';
    return false;
}

}  // namespace

int main() {
    VTerm* terminal = vterm_new(24, 80);
    if (terminal == nullptr) {
        std::cerr << "FAIL: vterm_new returned null.\n";
        return 1;
    }

    VTermState* state = vterm_obtain_state(terminal);
    if (state == nullptr) {
        std::cerr << "FAIL: vterm_obtain_state returned null.\n";
        vterm_free(terminal);
        return 1;
    }

    vterm_state_reset(state, 1);

    // This is OpenTUI's movement-disabled sequence. The tracking modes are
    // deliberately ordered for terminals where the last mode wins.
    constexpr char setup[] =
        "\x1b[?1003l\x1b[?1000h\x1b[?1002h\x1b[?1006h";
    const std::size_t setupSize = sizeof(setup) - 1;
    const std::size_t consumed =
        vterm_input_write(terminal, setup, setupSize);
    if (consumed != setupSize) {
        std::cerr << "FAIL: libvterm consumed " << consumed << " of "
                  << setupSize << " setup bytes.\n";
        vterm_free(terminal);
        return 1;
    }

    DrainOutput(terminal);

    constexpr char queryModes[] =
        "\x1b[?1000$p\x1b[?1002$p\x1b[?1003$p"
        "\x1b[?1006$p\x1b[?1007$p";
    vterm_input_write(terminal, queryModes, sizeof(queryModes) - 1);
    const std::string modeReply = DrainOutput(terminal);

    bool passed = true;
    constexpr char expectedModeReply[] =
        "\x1b[?1000;2$y\x1b[?1002;1$y\x1b[?1003;2$y"
        "\x1b[?1006;1$y\x1b[?1007;1$y";
    if (modeReply != expectedModeReply) {
        std::cerr << "FAIL: DEC mode replies\n"
                  << "  expected exactly: "
                  << DescribeBytes(expectedModeReply) << '\n'
                  << "  actual:           " << DescribeBytes(modeReply) << '\n';
        passed = false;
    }
    passed &= ExpectWheelPacket(
        terminal, 4, VTERM_MOD_NONE, "\x1b[<64;13;8M",
        "button 4 at row 7, column 12");
    passed &= ExpectWheelPacket(
        terminal, 5, VTERM_MOD_NONE, "\x1b[<65;13;8M",
        "button 5 at row 7, column 12");
    passed &= ExpectWheelPacket(
        terminal, 4, VTERM_MOD_SHIFT, "\x1b[<68;13;8M",
        "shift-modified wheel up");
    passed &= ExpectWheelPacket(
        terminal, 4, VTERM_MOD_ALT, "\x1b[<72;13;8M",
        "alt-modified wheel up");
    passed &= ExpectWheelPacket(
        terminal, 4, VTERM_MOD_CTRL, "\x1b[<80;13;8M",
        "control-modified wheel up");
    const VTermModifier allModifiers = static_cast<VTermModifier>(
        VTERM_MOD_SHIFT | VTERM_MOD_ALT | VTERM_MOD_CTRL);
    passed &= ExpectWheelPacket(
        terminal, 4, allModifiers, "\x1b[<92;13;8M",
        "fully modified wheel up");

    constexpr char teardown[] =
        "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l";
    vterm_input_write(terminal, teardown, sizeof(teardown) - 1);
    DrainOutput(terminal);
    passed &= ExpectWheelPacket(terminal, 4, VTERM_MOD_NONE, "",
                                "OpenTUI mouse teardown");

    vterm_free(terminal);
    return passed ? 0 : 1;
}
