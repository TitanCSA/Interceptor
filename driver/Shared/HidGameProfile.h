#pragma once
// USB-IF HID Usage Tables, application collections. Descriptor validation is
// still mandatory; these are protocol shapes, never a product allow-list.
#define IC_HID_PROFILE_DESKTOP 1u
#define IC_HID_PROFILE_SIMULATION 2u
#define IC_HID_PROFILE_SPORT 4u
#define IC_HID_PROFILE_GAME 8u
#define IC_HID_PROFILE_SPATIAL 16u
#define IC_HID_PROFILES_ALL 31u
static __inline unsigned IcHidGameProfile(unsigned short page,unsigned short usage) {
    if(page==1 && (usage==4 || usage==5 || usage==8))return IC_HID_PROFILE_DESKTOP;
    if(page==1 && usage==0x0F)return IC_HID_PROFILE_SPATIAL;
    if(page==2 && ((usage>=1 && usage<=12) || usage==0x20 || usage==0x21 || usage==0x24))return IC_HID_PROFILE_SIMULATION;
    if(page==4 && usage>=1 && usage<=4)return IC_HID_PROFILE_SPORT;
    if(page==5 && usage>=1 && usage<=3)return IC_HID_PROFILE_GAME;
    return 0;
}
