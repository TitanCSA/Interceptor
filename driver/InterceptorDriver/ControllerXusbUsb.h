#pragma once
// Same central capture ABI, with input intercepted below xusb22's policy layer.
#include "ControllerXusb.h"
NTSTATUS IcXusbUsbAdd(PWDFDEVICE_INIT init);
