#pragma once
// Build identity and immutable family boundary for this separate product.
// This header is also directly testable outside the kernel.
#define IC_PRODUCT_SERVICE L"Interceptor"
#define IC_PRODUCT_XUSB_HID_PARENT_VALUE L"InterceptorXusbHidParent"
#define IC_PRODUCT_XUSB_PARENT_VALUE L"InterceptorXusbParent"
#define IC_PRODUCT_CONTROL_HWID L"ROOT\\InterceptorControl"
#define IC_PRODUCT_NT_DEVICE L"\\Device\\Interceptor"
#define IC_PRODUCT_DOS_DEVICE L"\\DosDevices\\Interceptor"
#define IC_PRODUCT_TRIAL_SLOT 0u
#define IC_PRODUCT_TRIAL_KEY L"\\Registry\\Machine\\HARDWARE\\InterceptorTrial"
#ifndef IC_PRODUCT_INPUT_MASK
#define IC_PRODUCT_INPUT_MASK 3u
#endif
#ifndef IC_PRODUCT_CONTROLLER_MASK
#define IC_PRODUCT_CONTROLLER_MASK 15u
#endif
#ifndef IC_PRODUCT_HAS_KEYBOARD_MOUSE
#define IC_PRODUCT_HAS_KEYBOARD_MOUSE 1
#endif
#ifndef IC_PRODUCT_HAS_CONTROLLERS
#define IC_PRODUCT_HAS_CONTROLLERS 1
#endif
#define IC_PRODUCT_STREAM_MASK (IC_PRODUCT_INPUT_MASK | (IC_PRODUCT_CONTROLLER_MASK << 2))
#define IC_PRODUCT_HID_GUID {0x9ce34440,0x4716,0x4f39,{0x92,0x2b,0x99,0x5c,0x28,0xa5,0x2c,0x87}}
#define IC_PRODUCT_XUSB_GUID {0xf631b58e,0x571b,0x44ba,{0xa3,0x43,0x47,0xe2,0x25,0x7a,0x91,0xb3}}
#define IC_PRODUCT_XINPUTHID_GUID {0x2788ce3e,0x51bd,0x43eb,{0xa6,0x6d,0x13,0x74,0xb8,0x48,0xc5,0x8c}}
#define IC_PRODUCT_GIP_GUID {0x78310f48,0xccae,0x4a2d,{0x88,0x35,0xac,0xac,0xcf,0x26,0x6c,0xd2}}
#define IC_PRODUCT_XUSB_MIRROR_GUID {0x4bdfd98d,0x5b54,0x4ba0,{0x9e,0x75,0x0b,0xc7,0x37,0xa8,0xbd,0x51}}
static __inline int IcProductTypeAllowed(unsigned int type) {
    return type>=1 && type<=6 && (IC_PRODUCT_STREAM_MASK & (1u<<(type-1)))!=0;
}
static __inline int IcProductCreateMasksAllowed(unsigned int input,unsigned int controllers) {
    return !(input & ~IC_PRODUCT_INPUT_MASK) && !(controllers & ~IC_PRODUCT_CONTROLLER_MASK);
}
// Function numbers are the pointer-free protocol's CTL_CODE function field.
// Unknown/common functions keep the normal control dispatch validation.
static __inline int IcProductIoctlFunctionAllowed(unsigned int fn) {
    switch(fn) {
    case 0xA01:case 0xA03:case 0xA07:
        return (IC_PRODUCT_INPUT_MASK & 1u)!=0;
    case 0xA02:case 0xA04:case 0xA08:
        return (IC_PRODUCT_INPUT_MASK & 2u)!=0;
    case 0xA09:case 0xA0A:case 0xA0B:case 0xA0C:case 0xA0D:
        return (IC_PRODUCT_CONTROLLER_MASK & 1u)!=0;
    case 0xA10:case 0xA11:case 0xA12:case 0xA13:case 0xA14:case 0xA15:
        return (IC_PRODUCT_CONTROLLER_MASK & 2u)!=0;
    case 0xA17:case 0xA18:case 0xA19:case 0xA1A:case 0xA1B:
        return (IC_PRODUCT_CONTROLLER_MASK & 4u)!=0;
    case 0xA20:case 0xA21:case 0xA22:case 0xA23:case 0xA24:case 0xA25:
    case 0xA29:case 0xA2A:case 0xA2B:
        return (IC_PRODUCT_CONTROLLER_MASK & 8u)!=0;
    case 0xA30:case 0xA31:case 0xA32:case 0xA33:
        return IC_PRODUCT_HAS_CONTROLLERS;
    default:return 1;
    }
}

