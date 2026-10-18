#pragma once
#include "InterceptorProtocol.h"

// xusb22 assigns the IG component to the translated HID view; it is not
// necessarily 00, nor the ViGEm target index. Parse exactly two hex digits and
// a component boundary. Parent identity and descriptor validation remain
// separate mandatory checks; the group alone never associates two devices.
static __inline int IcXusbMirrorHex(uint16_t c) {
    if(c>='0'&&c<='9')return (int)(c-'0');
    if(c>='a'&&c<='f')return (int)(c-'a'+10);
    if(c>='A'&&c<='F')return (int)(c-'A'+10);
    return -1;
}
// Parse the product component only, never a substring or a truncated ID.
// VID/PID identify a device; the xusb22 service, wired compatible IDs and
// verified mirror descriptor establish its supported protocol separately.
static __inline int IcXusbProduct(const uint16_t* text,uint32_t chars,int usb,uint16_t* vid,uint16_t* pid) {
    const char* prefix=usb?"USB\\VID_":"HID\\VID_";
    uint32_t i;uint16_t v=0,p=0;
    if(!text || chars<21)return 0;
    for(i=0;i<21;++i) {
        uint16_t c=text[i];int digit;
        if(c>='a'&&c<='z')c=(uint16_t)(c-'a'+'A');
        if((i>=8 && i<12)||(i>=17 && i<21)) {
            digit=IcXusbMirrorHex(c);if(digit<0)return 0;
            if(i<12)v=(uint16_t)((v<<4)|digit);else p=(uint16_t)((p<<4)|digit);
        } else if(c!=(uint16_t)(i<8?prefix[i]:"&PID_"[i-12]))return 0;
    }
    if(chars>21 && text[21]!='&' && text[21]!='\\')return 0;
    if(vid)*vid=v;if(pid)*pid=p;return 1;
}
static __inline int IcXusbMirrorGroup(const uint16_t* text,uint32_t chars,int usb) {
    uint32_t i;int high,low;
    if(chars<27 || !IcXusbProduct(text,chars,usb,0,0))return -1;
    for(i=21;i<25;++i){uint16_t c=text[i];if(c>='a'&&c<='z')c=(uint16_t)(c-'a'+'A');if(c!=(uint16_t)"&IG_"[i-21])return -1;}
    high=IcXusbMirrorHex(text[25]);low=IcXusbMirrorHex(text[26]);
    if(high<0||low<0||(chars>27&&text[27]!='&'&&text[27]!='\\'))return -1;
    return high*16+low;
}
