#include "Interceptor.h"
#include <iostream>
#include <system_error>
using namespace Interceptor;

int main() try {
    std::cout
        << "Print and block keyboard input. Press Escape to stop and restore normal input.\n"
           "Run one example at a time as administrator. Close this window or press Ctrl+C to stop.\n"
        << std::flush;
    // Check installation and initialize supported devices before capturing.
    Get_Status();
    const auto init = Init_AllDevices();
    if (!init.ready) {
        std::cerr << "Initialization failed. Error: " << init.error
                  << ". Restart required: " << init.reboot_required << "\n";
        return 1;
    }
    DWORD consoleMode = 0; // Console only: prevent text selection from blocking console writes.
    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &consoleMode)) {
        SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),
                       (consoleMode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
    }
    for (;;) {
        // Receive: wait for input without polling.
        auto inputs = Get_Input_With_Timeout(INFINITE);
        for (auto& input : inputs) {
            if (input.Get<Keyboard_Input>()) {
                input.blocked = true;
            }
        }
        // Forward: send all unblocked inputs, including other device types.
        const auto sent = Send_Input(inputs);
        if (!sent) {
            throw std::system_error(sent.error, std::system_category(), "Send_Input");
        }
        for (auto& input : inputs) {
            if (auto* key = input.Get<Keyboard_Input>()) {
                if (key->stroke.make_code == 0x01 && !(key->stroke.flags & (KeyE0 | KeyE1))) {
                    return 0; // Escape
                }
                input.blocked = true; // Only this keyboard event is dropped.
                std::cout << "Keyboard " << key->device << ": scan=" << key->stroke.make_code << " "
                          << ((key->stroke.flags & KeyUp) ? "up" : "down") << '\n';
            }
        }
    }
} catch (const std::system_error& error) {
    std::cerr
        << "Driver error: " << error.code().value()
        << ". Install this edition with InterceptorSetup.exe, restart if requested, and run as administrator.\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
