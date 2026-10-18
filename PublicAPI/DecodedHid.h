#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Interceptor {
namespace Detail {struct Hid_Codec;struct Hid_Input_Access;}
enum class Hid_Value_Kind {Value,Axis,Hat,Trigger};
struct Hid_Value {
    std::uint16_t usage_page=0,usage=0,link_collection=0,bit_size=0,data_index=0;
    std::uint32_t element=0,units=0,units_exponent=0;
    std::uint8_t report_id=0;
    bool relative=false,has_null=false,is_null=false;
    Hid_Value_Kind kind=Hid_Value_Kind::Value;
    std::int64_t value=0,minimum=0,maximum=0,physical_minimum=0,physical_maximum=0;
    // Absolute axes: -1..1. Identified analog triggers: 0..1.
    // Relative, null and constant-range values have normalized_valid=false.
    double normalized=0;
    bool normalized_valid=false;
private:
    std::int64_t original_=0;
    double original_normalized_=0;
    std::uint32_t definition_=0; // Codec expansion is capped below 2^18.
    std::uint32_t original_element_=0;
    friend struct Detail::Hid_Codec;
};
struct Hid_Button {
    std::uint16_t usage_page=0,usage=0,link_collection=0,data_index=0;
    std::uint8_t report_id=0;
    bool pressed=false;
    bool trigger=false; // Explicit trigger usage or trigger collection.
private:
    bool original_=false;
    std::size_t definition_=0;
    friend struct Detail::Hid_Codec;
};
// Fields wider than 32 bits retain their exact packed little-endian bits.
struct Hid_Binary_Value {
    std::uint16_t usage_page=0,usage=0,link_collection=0,bit_size=0,data_index=0;
    std::uint32_t count=0;
    std::uint8_t report_id=0;
    std::vector<std::uint8_t> data;
private:
    std::vector<std::uint8_t> original_;
    std::size_t definition_=0;
    friend struct Detail::Hid_Codec;
};
// Each scalar occurs in exactly one list. Repeated X/Y usages remain separate
// axes with their report/collection/data-index/element identity intact.
struct Hid_State {
    std::vector<Hid_Value> axes;
    std::vector<Hid_Value> triggers;
    std::vector<Hid_Value> hats;
    std::vector<Hid_Value> values; // Other scalars, including vendor usages.
    std::vector<Hid_Button> buttons; // Includes released buttons in this report.
    std::vector<Hid_Binary_Value> binary_values;
};
struct Hid_Decoded_Input {
    std::string token;
    Hid_State state; // Current report only; never a guessed cumulative state.
    DWORD decode_error=ERROR_SUCCESS;
    std::uint32_t parser_status=0;
    bool decode_complete=false;
private:
    std::shared_ptr<const Detail::Hid_Codec> codec_;
    std::uint8_t report_id_=0;
    std::uint32_t source_id_=0;
    friend struct Detail::Hid_Codec;
    friend struct Detail::Hid_Input_Access;
};
}
