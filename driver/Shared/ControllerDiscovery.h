#pragma once
#include <wchar.h>
#include "HidGameProfile.h"
static __inline int IcNativeControllerService(const wchar_t* name) {
    return name && (!_wcsnicmp(name,L"xusb",4) || !_wcsnicmp(name,L"xinputhid",9) ||
                    !_wcsnicmp(name,L"xboxgip",7) || !_wcsnicmp(name,L"gip",3));
}
// Pure predicate shared with installer regression tests. A usage can occur in
// either hardware IDs or compatible IDs; do not assume Windows puts it in both.
static __inline unsigned IcControllerCollectionProfile(const wchar_t* id) {
    unsigned page=0,usage=0,i;
    if(!id || wcslen(id)!=25 || _wcsnicmp(id,L"HID_DEVICE_UP:",14) || _wcsnicmp(id+18,L"_U:",3))return 0;
    for(i=0;i<8;++i) {
        wchar_t c=id[(i<4?14:17)+i];unsigned digit;
        if(c>=L'0' && c<=L'9')digit=c-L'0';else if(c>=L'A' && c<=L'F')digit=c-L'A'+10;
        else if(c>=L'a' && c<=L'f')digit=c-L'a'+10;else return 0;
        if(i<4)page=(page<<4)|digit;else usage=(usage<<4)|digit;
    }
    return IcHidGameProfile((unsigned short)page,(unsigned short)usage);
}
static __inline int IcControllerCollectionId(const wchar_t* id) {
    return IcControllerCollectionProfile(id)!=0;
}
