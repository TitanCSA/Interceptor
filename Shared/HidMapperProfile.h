#pragma once
/* Shared by the HID mapper and the user-mode association preflight. */
#define IC_MAPPER_DESCRIPTOR_MAX 65536u
#define IC_MAPPER_REPORT_MAX 4096u
#define IC_MAPPER_VALUE_CAPS_MAX 512u
typedef enum IC_MAPPER_PROFILE_RESULT {
    IC_MAPPER_PROFILE_OK=0,
    IC_MAPPER_PROFILE_DESCRIPTOR_SIZE,
    IC_MAPPER_PROFILE_USAGE,
    IC_MAPPER_PROFILE_INPUT_SIZE,
    IC_MAPPER_PROFILE_VALUE_CAPS
} IC_MAPPER_PROFILE_RESULT;
/* expectedUsage=0 accepts either mapper; 2 means mouse, 6 means keyboard. */
static __inline IC_MAPPER_PROFILE_RESULT IcMapperProfile(unsigned long descriptorBytes,
    unsigned short usagePage,unsigned short usage,unsigned short inputBytes,
    unsigned short valueCaps,unsigned short expectedUsage) {
    if(!descriptorBytes || descriptorBytes>IC_MAPPER_DESCRIPTOR_MAX)return IC_MAPPER_PROFILE_DESCRIPTOR_SIZE;
    if(usagePage!=1 || (usage!=2 && usage!=6) || (expectedUsage && usage!=expectedUsage))return IC_MAPPER_PROFILE_USAGE;
    if(!inputBytes || inputBytes>IC_MAPPER_REPORT_MAX)return IC_MAPPER_PROFILE_INPUT_SIZE;
    if(valueCaps>IC_MAPPER_VALUE_CAPS_MAX)return IC_MAPPER_PROFILE_VALUE_CAPS;
    return IC_MAPPER_PROFILE_OK;
}
