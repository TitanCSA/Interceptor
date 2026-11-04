#define IC_SDK_INTERNAL 1
#pragma once
#include "ViewerBridge.h"
#include "Interceptor.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <array>
#include <vector>

namespace Viewer {
class HidDecoder {
    std::vector<uint8_t> descriptor_;
    std::vector<HIDP_BUTTON_CAPS> buttons_;
    std::vector<HIDP_VALUE_CAPS> values_;
    std::array<uint64_t,256> reportButtons_{};
    std::vector<uint8_t> buttonReportIds_;
    std::array<int,9> valueOwners_{}; // Eight axes + hat; first declared source.
    bool layoutPartial_=false;
    uint16_t inputBytes_=0;
    DWORD error_=ERROR_NOT_SUPPORTED;
public:
    explicit HidDecoder(std::vector<uint8_t> descriptor);
    DWORD Decode(const uint8_t* report,uint32_t bytes,VIEWER_DEVICE& state) noexcept;
};
DWORD DecodeKeyboard(const Interceptor::Keyboard_Input&,VIEWER_DEVICE&) noexcept;
DWORD DecodeMouse(const Interceptor::Mouse_Input&,VIEWER_DEVICE&) noexcept;
DWORD DecodeNative(const IC_NATIVE_GAMEPAD_STATE&,bool gip,VIEWER_DEVICE&) noexcept;
void Metadata(uint64_t sequence,uint64_t timestamp,VIEWER_DEVICE&) noexcept;
}
