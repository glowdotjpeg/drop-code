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

bool ExpectWheelPacket(VTerm* terminal, int button,
                       const char* expected, const char* caseName) {
    vterm_mouse_button_at(
        terminal, 7, 12, button, true, VTERM_MOD_NONE);

    const std::string actual = DrainOutput(terminal);
    if (actual == expected) {
        std::cout << "PASS: " << caseName << '\n';
        return true;
    }

    std::cerr << "FAIL: " << caseName << '\n'
              << "  expected exactly: " << DescribeBytes(expected) << '\n'
              << "  actual:           " << DescribeBytes(actual) << '\n'
              << "  The complete output must contain one SGR packet and no "
                 "preceding motion event.\n";
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

    constexpr char setup[] = "\x1b[?1003h\x1b[?1006h";
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

    bool passed = true;
    passed &= ExpectWheelPacket(
        terminal, 4, "\x1b[<64;13;8M", "button 4 at row 7, column 12");
    passed &= ExpectWheelPacket(
        terminal, 5, "\x1b[<65;13;8M", "button 5 at row 7, column 12");

    vterm_free(terminal);
    return passed ? 0 : 1;
}
