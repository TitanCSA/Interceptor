#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
#include <algorithm>
#include <charconv>
using namespace Interceptor;

int main(int argc, char** argv) try {
    std::cout << "Enter relative mouse X counts: 300 moves right, -485 moves left. Enter q to quit.\n"
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
    const auto console = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    if (!GetConsoleMode(console, &oldMode)) throw std::runtime_error("Open an interactive Windows console.");
    struct RestoreMode { HANDLE console; DWORD mode; ~RestoreMode() { SetConsoleMode(console, mode); } } restore{console, oldMode};
    if (!SetConsoleMode(console, ENABLE_PROCESSED_INPUT | ENABLE_EXTENDED_FLAGS)) throw std::runtime_error("Cannot configure the console.");
    const auto devices = Get_Devices();
    const auto mouse = std::find_if(devices.begin(), devices.end(), [](const auto& d) { return d.type == static_cast<unsigned>(Device_Type::Mouse); });
    if (mouse == devices.end()) throw std::runtime_error("Connect and associate a mouse first.");
    std::string number; bool tooLong = false;
    std::cout << "Raw counts are not screen pixels; Windows/games apply their sensitivity. Escape also exits.\nX > " << std::flush;
    for (;;) {
        { // The console handle wakes this wait too; no polling thread or idle timer is needed.
            auto inputs = Get_Input_With_Timeout(INFINITE, console);
            const auto sent = Send_Input(inputs); // Keep the physical keyboard/mouse working while typing.
            if (!sent) throw std::system_error(sent.error, std::system_category(), "Send_Input");
        }
        DWORD pending = 0;
        if (!GetNumberOfConsoleInputEvents(console, &pending)) throw std::runtime_error("Cannot read console input.");
        if (!pending) continue;
        INPUT_RECORD record{}; DWORD read = 0;
        if (!ReadConsoleInputW(console, &record, 1, &read)) throw std::runtime_error("Cannot read a console key.");
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        if (record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) return 0;
        for (WORD repeat = 0; repeat < record.Event.KeyEvent.wRepeatCount; ++repeat) {
        const auto character = record.Event.KeyEvent.uChar.UnicodeChar;
        if (character == L'\r') {
            if (number == "q" || number == "Q") return 0;
            std::int32_t delta = 0;
            const char* first = number.data(); const char* last = first + number.size();
            if (first != last && *first == '+') ++first;
            const auto parsed = std::from_chars(first, last, delta);
            const bool invalidPlus = !number.empty() && number[0] == '+' && (number.size() < 2 || number[1] < '0' || number[1] > '9');
            if (tooLong || invalidPlus || parsed.ec != std::errc{} || parsed.ptr != last) std::cout << "Enter a signed 32-bit whole number.\n";
            else {
                Mouse_Input movement{}; movement.device = mouse->id;
                auto& state = movement.stroke;
                state.x = delta; // A real relative mouse packet. Y, buttons and flags stay zero.
                const auto sent = Send_Input(movement);
                if (!sent || sent.accepted != 1) throw std::runtime_error("Could not send mouse movement.");
            }
            number.clear(); tooLong = false; std::cout << "\nX > " << std::flush;
        } else if (character == L'\b' && !number.empty()) { number.pop_back(); std::cout << "\b \b" << std::flush; }
        else if (character >= L' ' && character <= L'~') {
            if (number.size() < 32) { number += static_cast<char>(character); std::cout << static_cast<char>(character) << std::flush; }
            else tooLong = true;
        }
        }
    }
} catch (const std::system_error& error) {
    std::cerr << "Driver error: " << error.code().value() << ". Install this edition with InterceptorSetup.exe, restart if requested, and run as administrator.\n";
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
