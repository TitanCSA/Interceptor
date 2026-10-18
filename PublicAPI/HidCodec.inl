// Descriptor-driven SDK decoding. Never opens a device or starts a capture.
#include "HidCodec.h"
#include <cmath>
#include <limits>
#include <unordered_set>
namespace Interceptor::Detail {
namespace {
std::int64_t HidMinimum(const HIDP_VALUE_CAPS& c){return c.LogicalMin;}
std::int64_t HidMaximum(const HIDP_VALUE_CAPS& c){return c.LogicalMin<0?static_cast<std::int64_t>(c.LogicalMax):static_cast<std::uint32_t>(c.LogicalMax);}
std::int64_t HidSigned(std::uint32_t raw,unsigned bits,bool sign) {
    const auto mask=bits==32?0xffffffffull:(1ull<<bits)-1;
    const auto value=static_cast<std::uint64_t>(raw)&mask;
    return sign&&(value&(1ull<<(bits-1)))?static_cast<std::int64_t>(value)-static_cast<std::int64_t>(mask+1):static_cast<std::int64_t>(value);
}
double HidNormalized(std::int64_t value,std::int64_t minimum,std::int64_t maximum) {
    if(minimum<0&&maximum>0)return value<0?-static_cast<double>(value)/static_cast<double>(minimum):static_cast<double>(value)/static_cast<double>(maximum);
    return 2.0*static_cast<double>(value-minimum)/static_cast<double>(maximum-minimum)-1.0;
}
std::int64_t HidLogical(double value,std::int64_t minimum,std::int64_t maximum) {
    const double raw=minimum<0&&maximum>0?(value<0?-value*static_cast<double>(minimum):value*static_cast<double>(maximum)):
        static_cast<double>(minimum)+(value+1.0)*0.5*static_cast<double>(maximum-minimum);
    return static_cast<std::int64_t>(std::llround(raw));
}
std::uint32_t HidBits(const std::uint8_t* bytes,std::size_t bit,unsigned count) {
    // At most five byte loads for a possibly unaligned 32-bit field. No
    // unaligned typed loads, full-word overreads or host-endian assumptions.
    std::uint64_t raw=0;const auto shift=static_cast<unsigned>(bit%8);
    for(unsigned i=0;i<(shift+count+7)/8;++i)raw|=static_cast<std::uint64_t>(bytes[bit/8+i])<<(i*8);
    return static_cast<std::uint32_t>((raw>>shift)&((1ull<<count)-1));
}
std::uint32_t HidBits(const std::vector<std::uint8_t>& bytes,std::size_t bit,unsigned count) {return HidBits(bytes.data(),bit,count);}
void HidWriteBits(std::uint8_t* bytes,std::size_t bit,unsigned count,std::uint32_t value) {
    while(count) {
        const auto shift=static_cast<unsigned>(bit%8),take=(std::min)(8u-shift,count);
        const auto mask=static_cast<std::uint8_t>(((1u<<take)-1)<<shift);
        auto& byte=bytes[bit/8];byte=static_cast<std::uint8_t>((byte&~mask)|((value<<shift)&mask));
        value>>=take;bit+=take;count-=take;
    }
}
bool HidTriggerUsage(USHORT page,USHORT usage) {return page==2&&usage==0xC0;}
std::vector<std::uint8_t> HidTriggerCollections(const std::vector<HIDP_LINK_COLLECTION_NODE>& nodes) {
    // Resolve each parent edge once, including malformed cycles. Walking the
    // same deep ancestry for every usage would multiply descriptor setup cost.
    std::vector<std::uint8_t> flags(nodes.size());std::vector<USHORT> path;
    for(std::size_t root=0;root<nodes.size();++root) {
        if(flags[root])continue;
        path.clear();auto link=root;bool trigger=false;
        while(link<nodes.size()&&!flags[link]) {
            flags[link]=3;path.push_back(static_cast<USHORT>(link));const auto& node=nodes[link];
            if(node.LinkUsagePage==5&&node.LinkUsage==0x39){trigger=true;break;}
            if(!link||node.Parent==link)break;
            link=node.Parent;
        }
        if(link<nodes.size()&&flags[link]==2)trigger=true;
        for(const auto item:path)flags[item]=trigger?2:1;
    }
    return flags;
}
bool HidTriggerCollection(const std::vector<std::uint8_t>& flags,USHORT link) {return link<flags.size()&&flags[link]==2;}
DWORD HidParseError(NTSTATUS status){return status==HIDP_STATUS_NOT_IMPLEMENTED||status==HIDP_STATUS_NOT_VALUE_ARRAY?ERROR_NOT_SUPPORTED:ERROR_INVALID_DATA;}
}
std::shared_ptr<const Hid_Codec> Hid_Codec::Create(std::vector<std::uint8_t> bytes) {
    auto codec=std::make_shared<Hid_Codec>();codec->descriptor=std::move(bytes);codec->parser=hidParser;
    if(codec->descriptor.empty())Fail("Empty HID parser data",ERROR_INVALID_DATA);
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(codec->descriptor.data());
    if(codec->parser.caps(data,&codec->caps)!=HIDP_STATUS_SUCCESS||!codec->caps.InputReportByteLength||codec->caps.InputReportByteLength>IC_HID_REPORT_MAX)
        Fail("Invalid HID parser data",ERROR_INVALID_DATA);
    std::vector<HIDP_LINK_COLLECTION_NODE> collections(codec->caps.NumberLinkCollectionNodes);
    if(!collections.empty()) {
        ULONG count=static_cast<ULONG>(collections.size());
        if(codec->parser.collections(collections.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>collections.size())
            Fail("HID collection capabilities",ERROR_INVALID_DATA);
        collections.resize(count);
    }
    const auto triggerCollections=HidTriggerCollections(collections);
    codec->maxDataCount=codec->parser.maxData(HidP_Input,data);
    if(codec->maxDataCount>65536)Fail("Invalid HID data count",ERROR_INVALID_DATA);
    USHORT count=codec->caps.NumberInputValueCaps;std::vector<HIDP_VALUE_CAPS> values(count);
    if(count&&(codec->parser.values(HidP_Input,values.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>values.size()))Fail("HID value capabilities",ERROR_INVALID_DATA);
    values.resize(count);std::unordered_set<std::uint64_t> known;
    for(const auto& cap:values) {
        if(cap.IsAlias)continue;
        if(!cap.BitSize||!cap.ReportCount||
            (cap.IsRange&&(cap.Range.UsageMax<cap.Range.UsageMin||cap.Range.DataIndexMax<cap.Range.DataIndexMin||
             cap.Range.UsageMax-cap.Range.UsageMin!=cap.Range.DataIndexMax-cap.Range.DataIndexMin)))Fail("Invalid HID value range",ERROR_INVALID_DATA);
        const unsigned first=cap.IsRange?cap.Range.UsageMin:cap.NotRange.Usage,last=cap.IsRange?cap.Range.UsageMax:first;
        for(unsigned usage=first;usage<=last;++usage) {
            const auto index=cap.IsRange?static_cast<USHORT>(cap.Range.DataIndexMin+usage-first):cap.NotRange.DataIndex;
            if(!known.insert((static_cast<std::uint64_t>(cap.ReportID)<<32)|index).second)continue;
            if(codec->valueDefs.size()>=262144)Fail("HID capability expansion too large",ERROR_NOT_SUPPORTED);
            Hid_Value_Kind kind=Hid_Value_Kind::Value;
            if(HidTriggerUsage(cap.UsagePage,static_cast<USHORT>(usage))||HidTriggerCollection(triggerCollections,cap.LinkCollection))kind=Hid_Value_Kind::Trigger;
            else if(cap.UsagePage==1&&usage>=0x30&&usage<=0x38)kind=Hid_Value_Kind::Axis;
            else if(cap.UsagePage==1&&usage==0x39)kind=Hid_Value_Kind::Hat;
            codec->reports[cap.ReportID].values.push_back(codec->valueDefs.size());
            auto& reservation=codec->reports[cap.ReportID].counts[cap.BitSize>32?4:static_cast<std::size_t>(kind)];
            reservation+=cap.BitSize>32?1u:cap.IsRange?1u:cap.ReportCount;
            if(reservation>static_cast<std::size_t>(codec->caps.InputReportByteLength)*8)Fail("HID scalar expansion too large",ERROR_INVALID_DATA);
            Value definition{cap,static_cast<USHORT>(usage),index};definition.kind=kind;codec->valueDefs.push_back(definition);
        }
    }
    count=codec->caps.NumberInputButtonCaps;std::vector<HIDP_BUTTON_CAPS> buttons(count);
    if(count&&(codec->parser.buttons(HidP_Input,buttons.data(),&count,data)!=HIDP_STATUS_SUCCESS||count>buttons.size()))Fail("HID button capabilities",ERROR_INVALID_DATA);
    buttons.resize(count);known.clear();
    for(const auto& cap:buttons) {
        if(cap.IsAlias)continue;
        if(cap.IsRange&&(cap.Range.UsageMax<cap.Range.UsageMin||cap.Range.DataIndexMax<cap.Range.DataIndexMin||
            cap.Range.UsageMax-cap.Range.UsageMin!=cap.Range.DataIndexMax-cap.Range.DataIndexMin))Fail("Invalid HID button range",ERROR_INVALID_DATA);
        const unsigned first=cap.IsRange?cap.Range.UsageMin:cap.NotRange.Usage,last=cap.IsRange?cap.Range.UsageMax:first;
        for(unsigned usage=first;usage<=last;++usage) {
            const auto index=cap.IsRange?static_cast<USHORT>(cap.Range.DataIndexMin+usage-first):cap.NotRange.DataIndex;
            if(!known.insert((static_cast<std::uint64_t>(cap.ReportID)<<32)|index).second)continue;
            if(codec->buttonDefs.size()>=262144)Fail("HID button expansion too large",ERROR_NOT_SUPPORTED);
            codec->reports[cap.ReportID].buttons.push_back(codec->buttonDefs.size());codec->buttonDefs.push_back({cap.UsagePage,static_cast<USHORT>(usage),cap.LinkCollection,cap.ReportID,index,HidTriggerUsage(cap.UsagePage,static_cast<USHORT>(usage))||HidTriggerCollection(triggerCollections,cap.LinkCollection)});
        }
    }
    // Locate array bits once using synthetic tagged reports, never hardware I/O.
    // Windows' SetUsageValueArray rejects element sizes not divisible by eight.
    // Validated offsets allow exact encoding of those arrays as well.
    const auto reportBits=static_cast<unsigned>(codec->caps.InputReportByteLength)*8;
    for(auto& def:codec->valueDefs) {
        const auto& c=def.cap;const auto arrayCount=c.IsRange?1u:c.ReportCount;
        if(arrayCount==1&&c.BitSize<=32)continue;
        const auto bits=static_cast<unsigned>(c.BitSize)*arrayCount;
        if(!bits||bits>reportBits){def.array_status=HIDP_STATUS_INVALID_REPORT_LENGTH;continue;}
        if(std::any_of(codec->valueDefs.begin(),codec->valueDefs.end(),[&](const Value& other){return &other!=&def&&(!other.cap.IsRange&&other.cap.ReportCount>1)&&(!c.LinkCollection||other.cap.LinkCollection==c.LinkCollection)&&other.cap.ReportID==c.ReportID&&other.cap.UsagePage==c.UsagePage&&other.usage==def.usage;})){
            def.array_status=HIDP_STATUS_NOT_IMPLEMENTED;continue;
        }
        std::vector<std::uint8_t> probe(codec->caps.InputReportByteLength),packed((bits+7)/8);
        unsigned start=0,planes=0;while((1u<<planes)<reportBits)++planes;
        std::vector<std::vector<std::uint8_t>> patterns;
        for(unsigned plane=0;plane<planes;++plane) {
            std::fill(probe.begin(),probe.end(),std::uint8_t{});probe[0]=c.ReportID;
            for(unsigned bit=8;bit<reportBits;++bit)if((bit>>plane)&1u)probe[bit/8]|=static_cast<std::uint8_t>(1u<<(bit%8));
            std::fill(packed.begin(),packed.end(),std::uint8_t{});
            def.array_status=codec->parser.array(HidP_Input,c.UsagePage,c.LinkCollection,def.usage,reinterpret_cast<PCHAR>(packed.data()),static_cast<USHORT>(packed.size()),data,reinterpret_cast<PCHAR>(probe.data()),static_cast<ULONG>(probe.size()));
            if(def.array_status!=HIDP_STATUS_SUCCESS)break;
            if(packed[0]&1u)start|=1u<<plane;
            patterns.push_back(packed);
        }
        if(def.array_status!=HIDP_STATUS_SUCCESS)continue;
        if(start<8||start+bits>reportBits){def.array_status=HIDP_STATUS_INVALID_PREPARSED_DATA;continue;}
        for(unsigned plane=0;plane<planes;++plane)for(unsigned bit=0;bit<bits;++bit)
            if(HidBits(patterns[plane],bit,1)!=(((start+bit)>>plane)&1u))def.array_status=HIDP_STATUS_INVALID_PREPARSED_DATA;
        def.bit_offset=start;
    }
    return codec;
}
void Hid_Codec::Decode(const std::shared_ptr<const Hid_Codec>& codec,Hid_Decoded_Input& out,const std::uint8_t* report,std::uint32_t length,std::uint32_t source) {
    auto& state=out.state;state.axes.clear();state.triggers.clear();state.hats.clear();state.values.clear();state.buttons.clear();state.binary_values.clear();out.decode_complete=false;out.decode_error=ERROR_SUCCESS;out.parser_status=0;
    out.codec_=codec;out.source_id_=source;
    if(!codec||!report||length!=codec->caps.InputReportByteLength){out.decode_error=ERROR_INVALID_DATA;return;}
    out.report_id_=report[0];const auto& layout=codec->reports[report[0]];
    if(layout.values.empty()&&layout.buttons.empty()){out.decode_error=ERROR_NOT_SUPPORTED;return;}
    state.axes.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Axis)]);
    state.triggers.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Trigger)]);
    state.hats.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Hat)]);
    state.values.reserve(layout.counts[static_cast<std::size_t>(Hid_Value_Kind::Value)]);
    state.buttons.reserve(layout.buttons.size());state.binary_values.reserve(layout.counts[4]);
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint8_t*>(codec->descriptor.data()));
    auto buffer=reinterpret_cast<PCHAR>(const_cast<std::uint8_t*>(report));
    const auto failure=[&](NTSTATUS status){if(!out.decode_error){out.decode_error=HidParseError(status);out.parser_status=static_cast<std::uint32_t>(status);}};
    thread_local std::vector<HIDP_DATA> parsed;parsed.resize(codec->maxDataCount);ULONG parsedCount=codec->maxDataCount;
    const auto dataStatus=codec->maxDataCount?codec->parser.data(HidP_Input,parsed.data(),&parsedCount,data,buffer,length):HIDP_STATUS_SUCCESS;
    bool dataValid=dataStatus==HIDP_STATUS_SUCCESS&&parsedCount<=parsed.size();
    if(!dataValid){failure(dataStatus==HIDP_STATUS_SUCCESS?HIDP_STATUS_BUFFER_TOO_SMALL:dataStatus);parsedCount=0;}
    parsed.resize(parsedCount);
    const auto byIndex=[](const HIDP_DATA& a,const HIDP_DATA& b){return a.DataIndex<b.DataIndex;};
    if(!std::is_sorted(parsed.begin(),parsed.end(),byIndex))std::sort(parsed.begin(),parsed.end(),byIndex);
    if(std::adjacent_find(parsed.begin(),parsed.end(),[](const HIDP_DATA& a,const HIDP_DATA& b){return a.DataIndex==b.DataIndex;})!=parsed.end()) {
        failure(HIDP_STATUS_INVALID_PREPARSED_DATA);parsed.clear();dataValid=false;
    }
    std::size_t cursor=0;USHORT previous=0;
    const auto field=[&](USHORT index)->const HIDP_DATA* {
        // Sequential data-index queries are linear overall. Nonsequential
        // layouts use a bounded scan followed by logarithmic lookup.
        if(index<previous)cursor=0;previous=index;
        unsigned scanned=0;while(cursor<parsed.size()&&parsed[cursor].DataIndex<index&&scanned++<4)++cursor;
        if(cursor<parsed.size()&&parsed[cursor].DataIndex<index)cursor=static_cast<std::size_t>(std::lower_bound(parsed.begin()+cursor,parsed.end(),index,[](const HIDP_DATA& item,USHORT wanted){return item.DataIndex<wanted;})-parsed.begin());
        return cursor<parsed.size()&&parsed[cursor].DataIndex==index?&parsed[cursor]:nullptr;
    };
    for(const auto index:layout.values) {
        const auto& def=codec->valueDefs[index];const auto& c=def.cap;
        const unsigned count=c.IsRange?1u:c.ReportCount;
        const auto bits=static_cast<std::size_t>(count)*c.BitSize;
        if(bits>static_cast<std::size_t>(length)*8){failure(HIDP_STATUS_INVALID_REPORT_LENGTH);continue;}
        ULONG raw=0;NTSTATUS status=HIDP_STATUS_SUCCESS;
        if(count>1||c.BitSize>32){status=def.array_status;if(status==HIDP_STATUS_SUCCESS&&def.bit_offset+bits>static_cast<std::size_t>(length)*8)status=HIDP_STATUS_INVALID_REPORT_LENGTH;}
        else {const auto* value=field(def.index);if(value)raw=value->RawValue;else status=HIDP_STATUS_DATA_INDEX_NOT_FOUND;}
        if(status!=HIDP_STATUS_SUCCESS){failure(status);continue;}
        if(c.BitSize>32) {
            Hid_Binary_Value value;value.usage_page=c.UsagePage;value.usage=def.usage;value.link_collection=c.LinkCollection;value.report_id=c.ReportID;value.bit_size=c.BitSize;value.count=count;value.data_index=def.index;
            value.definition_=index;value.data.resize((bits+7)/8);
            for(std::size_t bit=0;bit<bits;bit+=8)value.data[bit/8]=static_cast<std::uint8_t>(HidBits(report,def.bit_offset+bit,static_cast<unsigned>((std::min)(std::size_t{8},bits-bit))));
            value.original_=value.data;state.binary_values.push_back(std::move(value));continue;
        }
        for(unsigned element=0;element<count;++element) {
            Hid_Value v;v.usage_page=c.UsagePage;v.usage=def.usage;v.link_collection=c.LinkCollection;v.report_id=c.ReportID;v.element=element;v.bit_size=c.BitSize;v.data_index=def.index;
            v.relative=!c.IsAbsolute;v.has_null=c.HasNull!=FALSE;v.minimum=HidMinimum(c);v.maximum=HidMaximum(c);
            v.physical_minimum=c.PhysicalMin;v.physical_maximum=c.PhysicalMin<0?static_cast<std::int64_t>(c.PhysicalMax):static_cast<std::uint32_t>(c.PhysicalMax);
            v.units=c.Units;v.units_exponent=c.UnitsExp;
            v.value=HidSigned(count>1?HidBits(report,def.bit_offset+static_cast<std::size_t>(element)*c.BitSize,c.BitSize):raw,c.BitSize,c.LogicalMin<0);
            v.original_=v.value;v.definition_=static_cast<std::uint32_t>(index);v.original_element_=element;v.is_null=v.has_null&&(v.value<v.minimum||v.value>v.maximum);
            v.kind=def.kind;
            const bool valid=!v.is_null&&v.value>=v.minimum&&v.value<=v.maximum;
            if(!v.is_null&&!valid)failure(HIDP_STATUS_VALUE_OUT_OF_RANGE);
            v.normalized_valid=valid&&!v.relative&&v.maximum>v.minimum&&(v.kind==Hid_Value_Kind::Axis||v.kind==Hid_Value_Kind::Trigger);
            if(v.normalized_valid)v.normalized=v.kind==Hid_Value_Kind::Trigger?
                static_cast<double>(v.value-v.minimum)/static_cast<double>(v.maximum-v.minimum):HidNormalized(v.value,v.minimum,v.maximum);
            v.original_normalized_=v.normalized;
            auto& destination=v.kind==Hid_Value_Kind::Axis?state.axes:v.kind==Hid_Value_Kind::Trigger?state.triggers:v.kind==Hid_Value_Kind::Hat?state.hats:state.values;
            destination.push_back(v);
        }
    }
    if(dataValid)for(const auto i:layout.buttons) {
        const auto& button=codec->buttonDefs[i];const auto* value=field(button.index);
        Hid_Button b;b.usage_page=button.page;b.usage=button.usage;b.link_collection=button.link;b.report_id=button.report;b.data_index=button.index;
        b.trigger=button.trigger;b.pressed=value&&value->On;b.original_=b.pressed;b.definition_=i;state.buttons.push_back(b);
    }
    out.decode_complete=out.decode_error==ERROR_SUCCESS;
}
DWORD Hid_Codec::Encode(const Hid_Decoded_Input& in,std::uint8_t* report,std::uint32_t length,std::uint32_t source) {
    const auto& state=in.state;
    if(!in.codec_) {
        if(!state.axes.empty()||!state.triggers.empty()||!state.hats.empty()||!state.values.empty()||!state.buttons.empty()||!state.binary_values.empty())return ERROR_INVALID_DATA;
        return ERROR_SUCCESS; // Explicit raw reports remain supported.
    }
    const auto& codec=*in.codec_;
    if(!report||length!=codec.caps.InputReportByteLength||report[0]!=in.report_id_||source!=in.source_id_)return ERROR_INVALID_DATA;
    auto data=reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<std::uint8_t*>(codec.descriptor.data()));auto buffer=reinterpret_cast<PCHAR>(report);
    thread_local std::vector<HIDP_DATA> scalarEdits;scalarEdits.clear();
    // Each control is authoritative in one dynamic state list. Only edited
    // fields are encoded; unknown bytes and explicit raw edits remain intact.
    for(const auto* list:{&state.axes,&state.triggers,&state.hats,&state.values})for(const auto& v:*list) {
        auto value=v.value;
        if(v.element!=v.original_element_)return ERROR_INVALID_DATA;
        if(value==v.original_&&v.normalized==v.original_normalized_)continue;
        if(v.definition_>=codec.valueDefs.size())return ERROR_INVALID_DATA;
        const auto& def=codec.valueDefs[v.definition_];const auto& c=def.cap;
        if(c.ReportID!=report[0]||!c.BitSize||c.BitSize>32||v.element!=v.original_element_||
            v.usage_page!=c.UsagePage||v.usage!=def.usage||v.link_collection!=c.LinkCollection||v.report_id!=c.ReportID||
            v.bit_size!=c.BitSize||v.data_index!=def.index||v.kind!=def.kind)return ERROR_INVALID_DATA;
        const auto minimum=HidMinimum(c),maximum=HidMaximum(c);
        if(v.normalized!=v.original_normalized_) {
            const bool trigger=def.kind==Hid_Value_Kind::Trigger;
            if((!trigger&&def.kind!=Hid_Value_Kind::Axis)||!c.IsAbsolute||maximum<=minimum||
                !v.normalized_valid||!std::isfinite(v.normalized)||v.normalized<(trigger?0.0:-1.0)||v.normalized>1.0)return ERROR_INVALID_PARAMETER;
            const auto normalized=trigger?static_cast<std::int64_t>(std::llround(static_cast<double>(minimum)+v.normalized*static_cast<double>(maximum-minimum))):HidLogical(v.normalized,minimum,maximum);
            if(value!=v.original_&&value!=normalized)return ERROR_INVALID_PARAMETER;
            value=normalized;
        }
        if(value==v.original_)continue;
        const auto representableMin=c.LogicalMin<0?-(1ll<<(c.BitSize-1)):0;
        const auto representableMax=c.LogicalMin<0?(1ll<<(c.BitSize-1))-1:static_cast<std::int64_t>((1ull<<c.BitSize)-1);
        if(value<representableMin||value>representableMax||(!c.HasNull&&(value<minimum||value>maximum)))return ERROR_INVALID_PARAMETER;
        const unsigned count=c.IsRange?1u:c.ReportCount;
        if(count>1) {
            const auto bits=static_cast<std::size_t>(count)*c.BitSize;if(v.element>=count||bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
            if(def.array_status!=HIDP_STATUS_SUCCESS||def.bit_offset+bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
            const auto bit=def.bit_offset+static_cast<std::size_t>(v.element)*c.BitSize;
            HidWriteBits(report,bit,c.BitSize,static_cast<std::uint32_t>(value));
        } else {HIDP_DATA item{};item.DataIndex=def.index;item.RawValue=static_cast<ULONG>(value);scalarEdits.push_back(item);}
    }
    if(!scalarEdits.empty()) {
        ULONG count=static_cast<ULONG>(scalarEdits.size());
        const auto status=codec.parser.setData(HidP_Input,scalarEdits.data(),&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS)return HidParseError(status);
        if(count!=scalarEdits.size())return ERROR_INVALID_DATA;
    }
    for(const auto& v:state.binary_values) {
        if(v.data==v.original_)continue;
        if(v.definition_>=codec.valueDefs.size()||v.data.size()!=v.original_.size())return ERROR_INVALID_DATA;
        const auto& def=codec.valueDefs[v.definition_];const auto& c=def.cap;
        const auto bits=static_cast<std::size_t>(c.IsRange?1u:c.ReportCount)*c.BitSize;
        if(c.ReportID!=report[0]||v.data.size()!=(bits+7)/8||bits>static_cast<std::size_t>(length)*8||
            v.usage_page!=c.UsagePage||v.usage!=def.usage||v.link_collection!=c.LinkCollection||v.report_id!=c.ReportID||
            v.bit_size!=c.BitSize||v.data_index!=def.index||v.count!=(c.IsRange?1u:c.ReportCount))return ERROR_INVALID_DATA;
        if(bits%8&&((v.data.back()^v.original_.back())&static_cast<std::uint8_t>(0xffu<<(bits%8))))return ERROR_INVALID_PARAMETER;
        if(def.array_status!=HIDP_STATUS_SUCCESS||def.bit_offset+bits>static_cast<std::size_t>(length)*8)return ERROR_INVALID_DATA;
        for(std::size_t n=0;n<bits;n+=8)HidWriteBits(report,def.bit_offset+n,static_cast<unsigned>((std::min)(std::size_t{8},bits-n)),v.data[n/8]);
    }
    // One scan: release immediately, queue presses until all slots are free.
    // The scalar scratch is no longer needed and can hold the pending presses.
    scalarEdits.clear();
    for(const auto& b:state.buttons) {
        if(b.pressed==b.original_)continue;
        if(b.definition_>=codec.buttonDefs.size())return ERROR_INVALID_DATA;
        const auto& def=codec.buttonDefs[b.definition_];
        if(def.report!=report[0]||b.usage_page!=def.page||b.usage!=def.usage||b.link_collection!=def.link||
            b.report_id!=def.report||b.data_index!=def.index||b.trigger!=def.trigger)return ERROR_INVALID_DATA;
        HIDP_DATA item{};item.DataIndex=def.index;item.On=b.pressed?TRUE:FALSE;
        if(b.pressed){scalarEdits.push_back(item);continue;}
        ULONG count=1;
        const auto status=codec.parser.setData(HidP_Input,&item,&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS&&status!=HIDP_STATUS_BUTTON_NOT_PRESSED)return HidParseError(status);
        if(status==HIDP_STATUS_SUCCESS&&count!=1)return ERROR_INVALID_DATA;
    }
    if(!scalarEdits.empty()) {
        ULONG count=static_cast<ULONG>(scalarEdits.size());
        const auto status=codec.parser.setData(HidP_Input,scalarEdits.data(),&count,data,buffer,length);
        if(status!=HIDP_STATUS_SUCCESS)return HidParseError(status);
        if(count!=scalarEdits.size())return ERROR_INVALID_DATA;
    }
    return ERROR_SUCCESS;
}
}
