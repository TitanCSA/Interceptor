#include "Interceptor.h"
#include <iostream>
#include <string_view>
#include <system_error>
#include <algorithm>
#include <cmath>
using namespace Interceptor;
// HID labels are generic: auto uses X/Y + Z/Rz when present, otherwise X/Y + Rx/Ry.
// --hid-layout xy-rxry or xy-zrz resolves ambiguous descriptors without VID/PID rules.
bool HasAxis(const Hid_State& state, const Hid_Value& axis, unsigned usage) {
    for (const auto& other : state.axes)
        if (other.usage_page == 1 && other.usage == usage && other.report_id == axis.report_id &&
            other.link_collection == axis.link_collection && other.element == axis.element) return true;
    return false;
}
int AxisRole(const Hid_State& state, const Hid_Value& axis, std::string_view layout) {
    if (axis.usage_page != 1) return 0;
    if (axis.usage == 0x30) return 1; // Horizontal X
    if (axis.usage == 0x31) return 2; // Vertical Y, including repeated Y controls
    const bool zr = layout == "xy-zrz" || (layout == "auto" && HasAxis(state, axis, 0x32) && HasAxis(state, axis, 0x35));
    if (axis.usage == (zr ? 0x32 : 0x33)) return 1; // Right stick horizontal
    if (axis.usage == (zr ? 0x35 : 0x34)) return 2; // Right stick vertical
    if ((layout != "auto" || zr) && (axis.usage == (zr ? 0x33 : 0x32) || axis.usage == (zr ? 0x34 : 0x35))) return 3;
    return 0; // Leave unknown/combined-trigger encodings untouched.
}
bool Editable(const Hid_Value& axis) { return axis.normalized_valid && !axis.relative && !axis.is_null; }
double Unit(short value) { return value < 0 ? value / 32768.0 : value / 32767.0; }
short Raw(double value) { value = std::clamp(value, -1.0, 1.0); return static_cast<short>(std::lround(value * (value < 0 ? 32768.0 : 32767.0))); }
double exponent = 2.0; // Inverse of a matching game exponent; 2 means square root.
double Curve(double x) { return std::copysign(std::pow(std::abs(x), 1.0 / exponent), x); }
void Change(Hid_State& state, std::string_view layout) {
    for (auto& axis : state.axes) {
        const auto role = AxisRole(state, axis, layout);
        if (Editable(axis) && (role == 1 || role == 2)) axis.normalized = Curve(axis.normalized);
    }
}
void Change(IC_NATIVE_GAMEPAD_STATE& state, bool) {
    state.left_x = Raw(Curve(Unit(state.left_x))); state.left_y = Raw(Curve(Unit(state.left_y)));
    state.right_x = Raw(Curve(Unit(state.right_x))); state.right_y = Raw(Curve(Unit(state.right_y)));
}

int main(int argc, char** argv) try {
    std::cout << "Apply an inverse exponential stick curve (square root by default). Supports all families.\n"
                 "Run one example at a time as administrator. Close this window or press Ctrl+C to stop.\n" << std::flush;
    if (argc == 2 && std::string_view(argv[1]) == "--help") return 0;
    std::string_view layout = "auto";
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--hid-layout" && i + 1 < argc) layout = argv[++i];
        else if (std::string_view(argv[i]) == "--exponent" && i + 1 < argc) {
            std::size_t used = 0; const std::string value = argv[++i]; exponent = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(exponent) || exponent < 1 || exponent > 16) throw std::invalid_argument("Exponent must be between 1 and 16.");
        }
        else throw std::invalid_argument("Use --hid-layout auto|xy-rxry|xy-zrz, or --help.");
    }
    if (layout != "auto" && layout != "xy-rxry" && layout != "xy-zrz") throw std::invalid_argument("Unknown HID layout.");
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
        for (auto& input : inputs) {
            if (auto* hid = input.Get<Controller_Hid_Input>()) Change(hid->state, layout);
            else if (auto* xinput = input.Get<Controller_Xinput_Input>()) Change(xinput->state, false);
            else if (auto* xinputHid = input.Get<Controller_XinputHid_Input>()) Change(xinputHid->state, layout);
            else if (auto* gip = input.Get<Controller_GIP_Input>()) Change(gip->state, true);
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
