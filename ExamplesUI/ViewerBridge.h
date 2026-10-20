#define IC_SDK_INTERNAL 1
#pragma once
#include <Windows.h>
#include <stdint.h>
#include <stddef.h>
#include "Interceptor.h"

#ifdef VIEWER_BRIDGE_EXPORTS
#define VIEWER_API extern "C" __declspec(dllexport)
#else
#define VIEWER_API extern "C" __declspec(dllimport)
#endif
#define VIEWER_VERSION 1u
#define VIEWER_MAX_DEVICES 256u
enum VIEWER_STATUS : uint32_t {
    ViewerStopped = 0,
    ViewerStarting = 1,
    ViewerRunning = 2,
    ViewerNoDevices = 3,
    ViewerRebootRequired = 4,
    ViewerBackendUnavailable = 5,
    ViewerError = 6,
    ViewerRevoked = 7,
    ViewerPreparing = 8,
    ViewerPreparationTimeout = 9,
    ViewerTrialExpired = 10
};
enum VIEWER_STATE_FLAGS : uint32_t {
    ViewerHasInput = 1,
    ViewerDecodeUnsupported = 2,
    ViewerDecodeError = 4,
    ViewerHasHat = 8,
    ViewerHasTriggers = 16,
    ViewerNativeButtons = 32,
    ViewerPartialDecode = 64
};
enum VIEWER_STAGE : uint32_t {
    ViewerStageNone = 0,
    ViewerStageInitialize = 1,
    ViewerStageCapabilities = 2,
    ViewerStageCreate = 3,
    ViewerStageEnumerate = 4,
    ViewerStageWait = 5,
    ViewerStageReceive = 6,
    ViewerStageForward = 7,
    ViewerStageDecode = 8
};
#pragma pack(push, 8)
struct VIEWER_DIAGNOSTIC {
    uint32_t size, version, stage, type, device, requested, accepted, win32Error, resetReason, readyMask;
    uint64_t sequence, generation, received, forwarded;
    wchar_t message[512];
};
struct VIEWER_SNAPSHOT {
    uint32_t size, version, status, win32Error, resetReason, deviceCount, mask, controllers;
    uint64_t revision, generation, received, forwarded;
    uint32_t initAssociated, initExisting;
    uint64_t reserved;
};
struct VIEWER_DEVICE {
    IC_DEVICE_INFO info; // 800 bytes; UTF-16 name[128], hardware_id[256].
    uint64_t sequence, timestamp, inputCount;
    uint32_t stateFlags, decodeError;               // Decode error is Win32 or HIDP NTSTATUS.
    uint8_t keys[96];                               // scan, 256+E0 scan, 512+E1 scan; bit per key.
    uint32_t mouseButtons;                          // Bits0..4: left,right,middle,X1,X2.
    int32_t mouseX, mouseY, wheel, horizontalWheel; // Latest packet (not accumulated motion).
    uint32_t mouseFlags;                            // SDK MouseAbsolute / MouseVirtualDesktop.
    uint64_t buttons;                               // HID usage1=>bit0; native raw bits + Guide bit16.
    float axes[8];                                  // [-1,1]; X,Y,Z,Rx,Ry,Rz,Slider,Dial.
    int32_t rawAxes[8];
    uint16_t axisUsages[8];          // Generic Desktop usages0x30..0x37.
    uint32_t axisMask;               // Only bits set here are valid.
    int32_t hat;                     // -1 neutral;0..7 N,NE,E,SE,S,SW,W,NW.
    float leftTrigger, rightTrigger; // [0,1], only when ViewerHasTriggers.
    uint64_t buttonMask;             // Known button bits; same convention as buttons.
};
#pragma pack(pop)
static_assert(sizeof(VIEWER_SNAPSHOT) == 80, "Viewer snapshot ABI");
static_assert(sizeof(VIEWER_DIAGNOSTIC) == 1096 && offsetof(VIEWER_DIAGNOSTIC, message) == 72,
              "Viewer diagnostic ABI");
static_assert(sizeof(VIEWER_DEVICE) == 1064, "Viewer device ABI");
static_assert(offsetof(VIEWER_DEVICE, keys) == 832 && offsetof(VIEWER_DEVICE, buttons) == 952,
              "Viewer state offsets");

// Exactly one capture session per DLL/process. Start is asynchronous. The event
// must be an auto-reset event; Start duplicates it, and the caller may close its
// own handle after Start returns. Start returns ERROR_BUSY if already started.
// Init_AllDevices() runs in the worker; it never displays a restart dialog.
VIEWER_API DWORD __cdecl Viewer_Start(uint8_t mask, uint8_t controllers, HANDLE changeEvent) noexcept;
// Signal stop and join outside the snapshot lock. May wait for SDK initialization
// already underway; call outside the UI thread. Call before unloading this DLL.
VIEWER_API void __cdecl Viewer_Stop() noexcept;
// Header.size must equal80 and version1. Header is returned even on122. Device
// array may be null only when capacity=0. Required is always written on success
// or122. A successful copy acknowledges the coalesced notification atomically.
VIEWER_API DWORD __cdecl Viewer_ReadSnapshot(VIEWER_SNAPSHOT *header, VIEWER_DEVICE *devices,
                                             uint32_t capacity, uint32_t *required) noexcept;
// Read-only latched failure; never opens a driver or creates a context. Stop
// retains this evidence; a new Start clears it. Initialize size1096/version1.
// Type/device identify the failing operation when known. During a wait-stage
// revocation they are zero: the last input is not proof of which device reset.
VIEWER_API DWORD __cdecl Viewer_ReadDiagnostic(VIEWER_DIAGNOSTIC *diagnostic) noexcept;
// Pure decoder entry points for software/lab tests. Never initialize devices,
// create a capture, inject inputs, or access global viewer state. Raw input must
// have the exact SDK ABI size for type. HID needs trusted SDK preparsed data;
// it is not an arbitrary report-descriptor parser. State is updated in place.
VIEWER_API DWORD __cdecl Viewer_DecodeInput(uint32_t type, const void *input, uint32_t inputBytes,
                                            const void *preparsed, uint32_t preparsedBytes,
                                            VIEWER_DEVICE *state) noexcept;
// Runs pure descriptor-policy fixtures against the same rules used by HidP
// decoding. It does not fabricate preparsed data or access a device.
VIEWER_API DWORD __cdecl Viewer_TestHidRules(uint32_t *assertions) noexcept;
