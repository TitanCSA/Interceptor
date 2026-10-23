#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
#include <algorithm>
using namespace Interceptor;

int main(int argc, char** argv) try {
    std::cout << "Invert mouse Y. X, buttons, wheels and other devices stay unchanged.\n"
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
        for (auto& input : inputs) if (auto* mouse = input.Get<Mouse_Input>()) {
            auto& y = mouse->stroke.y;
            if (mouse->stroke.flags & MouseAbsolute) y = 65535 - std::clamp(y, 0, 65535);
            else y = y == INT32_MIN ? INT32_MAX : -y; // Relative motion; guard the signed minimum.
        }
        const auto sent = Send_Input(inputs); // Forward every unblocked entry, including other device types.
        if (!sent) throw std::system_error(sent.error, std::system_category(), "Send_Input");
    }
} catch (const std::system_error& error) {
    std::cerr << "Driver error: " << error.code().value() << ". Install this edition with InterceptorSetup.exe, restart if requested, and run as administrator.\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
