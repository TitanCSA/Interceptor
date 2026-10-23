#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
#include <map>
#include <set>
using namespace Interceptor;
bool Modifier(const Keyboard_Input& key) {
    return !(key.stroke.flags & KeyE1) && (key.stroke.make_code == 0x1D ||
        (!(key.stroke.flags & KeyE0) && (key.stroke.make_code == 0x2A || key.stroke.make_code == 0x36)));
}

int main(int argc, char** argv) try {
    std::cout << "Ctrl+H types hello. The shortcut H is consumed; other keys keep working.\n"
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
    std::map<std::uint64_t, Keyboard_Input> held;
    std::set<std::uint32_t> consumedH;
    bool capsLock = (GetKeyState(VK_CAPITAL) & 1) != 0;
    for (;;) {
        auto inputs = Get_Input_With_Timeout(INFINITE);
        for (auto& input : inputs) {
            if (auto* key = input.Get<Keyboard_Input>()) {
                const auto code = key->stroke.make_code;
                const bool up = (key->stroke.flags & KeyUp) != 0;
                const auto id = (std::uint64_t(key->device) << 32) | ((key->stroke.flags & (KeyE0 | KeyE1)) << 16) | code;
                const bool repeat = held.count(id) != 0;
                if (up) held.erase(id); else held[id] = *key;
                if (code == 0x3A && !(key->stroke.flags & (KeyE0 | KeyE1)) && !up && !repeat) capsLock = !capsLock;
                bool control = false, altOrWindows = false;
                for (const auto& entry : held) {
                    const auto& stroke = entry.second.stroke;
                    control |= stroke.make_code == 0x1D && !(stroke.flags & KeyE1);
                    altOrWindows |= stroke.make_code == 0x38 || ((stroke.flags & KeyE0) && (stroke.make_code == 0x5B || stroke.make_code == 0x5C));
                }
                if (code == 0x23 && !(key->stroke.flags & (KeyE0 | KeyE1))) { // H
                    if (consumedH.count(key->device)) { input.blocked = true; if (up) consumedH.erase(key->device); }
                    else if (!up && !repeat && control && !altOrWindows) {
                        consumedH.insert(key->device); input.blocked = true;
                        std::vector<Keyboard_Input> text;
                        auto emit = [&](std::uint16_t scan, bool release) {
                            auto letter = *key; letter.stroke.make_code = scan; letter.stroke.flags = release ? KeyUp : 0;
                            text.push_back(letter);
                        };
                        // Temporarily release Ctrl/Shift so hello is text, then restore their physical state.
                        for (const auto& entry : held) if (Modifier(entry.second)) {
                            auto release = entry.second; release.stroke.flags |= KeyUp; text.push_back(release);
                        }
                        if (capsLock) emit(0x2A, false);
                        for (const auto scan : {0x23, 0x12, 0x26, 0x26, 0x18}) { emit(static_cast<std::uint16_t>(scan), false); emit(static_cast<std::uint16_t>(scan), true); }
                        if (capsLock) emit(0x2A, true);
                        for (const auto& entry : held) if (Modifier(entry.second)) text.push_back(entry.second);
                        const auto sent = Send_Input(text);
                        if (!sent || sent.accepted != text.size()) throw std::runtime_error("Could not send the complete hello sequence.");
                    }
                }
            }
            const auto sent = Send_Input(input); // Keep each generated sequence in order with physical events.
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
