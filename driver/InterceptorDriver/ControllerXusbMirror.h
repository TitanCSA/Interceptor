#pragma once
#include "ControllerXusb.h"
#include "../Shared/ControllerXusbMirror.h"
void IcXusbMirrorInitialize(void);
NTSTATUS IcXusbMirrorAdd(PWDFDEVICE_INIT init);
// All hooks below run at PASSIVE_LEVEL under gGate. Parent is the exact XUSB
// devnode instance string, not a product ID or the Windows XInput slot number.
BOOLEAN IcXusbMirrorReadyLocked(PCUNICODE_STRING parent);
NTSTATUS IcXusbMirrorCaptureLocked(PCUNICODE_STRING parent);
void IcXusbMirrorResetLocked(void);
NTSTATUS IcXusbMirrorSendLocked(PCUNICODE_STRING parent,const IC_NATIVE_GAMEPAD_STATE* state,BOOLEAN owned);
void IcXusbMirrorPhysicalLocked(PCUNICODE_STRING parent,const IC_NATIVE_GAMEPAD_STATE* state);
