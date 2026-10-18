#pragma once
#include "InputTypes.h"
#include <hidsdi.h>
#include <hidpi.h>
#include <array>
namespace Interceptor::Detail {
// The replaceable parser boundary is used by pure tests. Production uses HidP.
struct Hid_Parser {
    decltype(&HidP_GetCaps) caps=HidP_GetCaps;
    decltype(&HidP_GetValueCaps) values=HidP_GetValueCaps;
    decltype(&HidP_GetButtonCaps) buttons=HidP_GetButtonCaps;
    decltype(&HidP_GetUsageValueArray) array=HidP_GetUsageValueArray;
    decltype(&HidP_GetData) data=HidP_GetData;
    decltype(&HidP_SetData) setData=HidP_SetData;
    decltype(&HidP_MaxDataListLength) maxData=HidP_MaxDataListLength;
    decltype(&HidP_GetLinkCollectionNodes) collections=HidP_GetLinkCollectionNodes;
};
inline Hid_Parser hidParser;
struct Hid_Codec {
    struct Value {HIDP_VALUE_CAPS cap;USHORT usage,index;std::uint32_t bit_offset=0;NTSTATUS array_status=HIDP_STATUS_SUCCESS;Hid_Value_Kind kind=Hid_Value_Kind::Value;};
    struct Button {USHORT page,usage,link;UCHAR report;USHORT index;bool trigger=false;};
    struct Report {
        std::vector<std::size_t> values,buttons;
        // Exact output reservations avoid geometric allocation/copy growth on
        // freshly returned public inputs. Index by Hid_Value_Kind, then binary.
        std::array<std::size_t,5> counts{};
    };
    std::vector<std::uint8_t> descriptor;
    std::vector<Value> valueDefs;
    std::vector<Button> buttonDefs;
    std::array<Report,256> reports;
    HIDP_CAPS caps{};
    ULONG maxDataCount=0;
    Hid_Parser parser;
    static std::shared_ptr<const Hid_Codec> Create(std::vector<std::uint8_t>);
    static void Decode(const std::shared_ptr<const Hid_Codec>&,Hid_Decoded_Input&,const std::uint8_t*,std::uint32_t,std::uint32_t);
    static DWORD Encode(const Hid_Decoded_Input&,std::uint8_t*,std::uint32_t,std::uint32_t);
};
}
