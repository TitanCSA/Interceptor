#pragma once
// HIDP_CAPS / HIDP_*_CAPS must be available from hidpi.h. This profile is the
// Windows xusb22 gamepad legacy collection observed on the supported 028E
// transport. No byte offsets are used; Send uses the HID parser's setters.
#define IC_XUSB_MIRROR_REPORT_BYTES 15u
static __inline int IcXusbMirrorProfileValid(const HIDP_CAPS* caps,const HIDP_VALUE_CAPS* values,
                                          unsigned valueCount,const HIDP_BUTTON_CAPS* buttons,unsigned buttonCount) {
    unsigned i,seen=0;
    if(!caps || !values || !buttons || caps->UsagePage!=1 || caps->Usage!=5 ||
       caps->InputReportByteLength!=IC_XUSB_MIRROR_REPORT_BYTES || caps->NumberInputValueCaps!=6 ||
       caps->NumberInputButtonCaps!=1 || valueCount!=6 || buttonCount!=1)return 0;
    for(i=0;i<valueCount;++i) {
        const HIDP_VALUE_CAPS* v=values+i;unsigned bit;USHORT link;
        if(v->IsRange || v->IsAlias || v->ReportID!=0 || v->UsagePage!=1 || !v->IsAbsolute || v->ReportCount!=1)return 0;
        switch(v->NotRange.Usage) {
        case 0x30:bit=1;link=1;break;
        case 0x31:bit=2;link=1;break;
        case 0x33:bit=4;link=2;break;
        case 0x34:bit=8;link=2;break;
        case 0x32:bit=16;link=3;break;
        case 0x39:bit=32;link=0;break;
        default:return 0;
        }
        if((seen&bit) || v->LinkCollection!=link)return 0;seen|=bit;
        if(bit==32) {
            if(v->BitSize!=4 || v->LogicalMin!=1 || v->LogicalMax!=8 || !v->HasNull)return 0;
        } else if(v->BitSize!=16 || v->LogicalMin!=0 || (v->LogicalMax!=-1 && v->LogicalMax!=65535) || v->HasNull)return 0;
    }
    return seen==63 && buttons->ReportID==0 && buttons->UsagePage==9 && buttons->LinkCollection==0 &&
        buttons->IsRange && !buttons->IsAlias && buttons->IsAbsolute && buttons->Range.UsageMin==1 && buttons->Range.UsageMax==10;
}
