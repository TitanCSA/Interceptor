#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
#include <algorithm>
#include <set>
using namespace Interceptor;
void EmitKey(std::uint16_t scan, bool up) {
    Keyboard_Input key{}; // Device 0 routes to the first captured keyboard.
    key.stroke.make_code = scan; key.stroke.flags = up ? KeyUp : 0;
    const auto sent = Send_Input(key);
    if (!sent || sent.accepted != 1) throw std::runtime_error("Could not send a generated key.");
}

int main(int argc, char** argv) try {
    std::cout << "Invert mouse X. Hold J or mouse X1/X2 to hold H+K instead.\n"
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
    std::set<std::uint64_t> physicalH, physicalK, triggers;
    bool outputH = false, outputK = false;
    std::vector<Input_Event*> order; order.reserve(256);
    for (;;) {
        auto inputs = Get_Input_With_Timeout(INFINITE);
        order.clear(); for (auto& input : inputs) order.push_back(&input);
        std::sort(order.begin(), order.end(), [](const auto* a, const auto* b) { return a->sequence < b->sequence; });
        for (auto* event : order) { // Mouse and keyboard triggers share chronological order.
            auto& input = *event;
            bool repeatH = false, repeatK = false;
            if (auto* key = input.Get<Keyboard_Input>()) {
                const auto code = key->stroke.make_code;
                if (!(key->stroke.flags & (KeyE0 | KeyE1)) && (code == 0x23 || code == 0x24 || code == 0x25)) {
                    auto& held = code == 0x23 ? physicalH : code == 0x25 ? physicalK : triggers;
                    const auto id = std::uint64_t(key->device) << 16;
                    const bool up = (key->stroke.flags & KeyUp) != 0, repeat = !up && held.count(id);
                    if (up) held.erase(id); else held.insert(id);
                    repeatH = repeat && code != 0x25; repeatK = repeat && code != 0x23;
                    input.blocked = true; // H/K are emitted as one balanced aggregate; J itself is dropped.
                }
            } else if (auto* mouse = input.Get<Mouse_Input>()) {
                auto& x = mouse->stroke.x;
                if (mouse->stroke.flags & MouseAbsolute) x = 65535 - std::clamp(x, 0, 65535);
                else x = x == INT32_MIN ? INT32_MAX : -x;
                for (unsigned side = 0; side < 2; ++side) {
                    const auto id = (std::uint64_t(mouse->device) << 16) | (side + 1);
                    if (mouse->stroke.button_flags & (0x40 << (side * 2))) triggers.insert(id);
                    if (mouse->stroke.button_flags & (0x80 << (side * 2))) triggers.erase(id);
                }
                mouse->stroke.button_flags &= 0xFC3F; // Consume X1/X2, preserving movement and other buttons.
            }
            const bool wantH = !physicalH.empty() || !triggers.empty(), wantK = !physicalK.empty() || !triggers.empty();
            if (wantH != outputH || repeatH) EmitKey(0x23, !wantH);
            if (wantK != outputK || repeatK) EmitKey(0x25, !wantK);
            outputH = wantH; outputK = wantK;
            const auto sent = Send_Input(input);
            if (!sent) throw std::system_error(sent.error, std::system_category(), "Send_Input");
        }
    }
} catch (const std::system_error& error) {
    std::cerr << "Driver error: " << error.code().value() << ". Install this edition with InterceptorSetup.exe, restart if requested, and run as administrator.\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
