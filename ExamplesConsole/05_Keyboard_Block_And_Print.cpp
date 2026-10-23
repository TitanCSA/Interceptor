#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
using namespace Interceptor;

int main(int argc, char** argv) try {
    std::cout << "Print and block keyboard input. Press Escape to stop and restore normal input.\n"
                 "Run one example at a time as administrator. Close this window or press Ctrl+C to stop.\n" << std::flush;
    if (argc == 2 && std::string_view(argv[1]) == "--help") return 0;
    if (argc != 1) throw std::invalid_argument("Run without arguments, or use --help.");
    Get_Status(); // Read-only check: this edition's installed driver must respond.
    const auto init = Init_AllDevices();
    if (!init.ready) {
        std::cerr << "Initialization failed. Error: " << init.error << ". Restart required: " << init.reboot_required << "\n";
        return 1;
    }
    DWORD consoleMode = 0; // Avoid Quick Edit pausing capture in the classic Windows console.
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &consoleMode))
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), (consoleMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
    for (;;) {
        auto inputs = Get_Input_With_Timeout(INFINITE); // Wait for a batch; the library owns the capture.
        for (auto& input : inputs) if (input.Get<Keyboard_Input >()) input.blocked = true;
        const auto sent = Send_Input(inputs); // Forward every unblocked entry, including other device types.
        if (!sent) throw std::system_error(sent.error, std::system_category(), "Send_Input");
        for (auto& input : inputs) if (auto* key = input.Get<Keyboard_Input>()) {
            if (key->stroke.make_code == 0x01 && !(key->stroke.flags & (KeyE0 | KeyE1))) return 0; // Escape
            input.blocked = true; // Only this keyboard event is dropped.
            std::cout << "Keyboard " << key->device << ": scan=" << key->stroke.make_code
                      << " " << ((key->stroke.flags & KeyUp) ? "up" : "down") << '\n';
        }
    }
} catch (const std::system_error& error) {
    std::cerr << "Driver error: " << error.code().value() << ". Install this edition with InterceptorSetup.exe, restart if requested, and run as administrator.\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
