#pragma once
#include "ControllerNative.h"

// Optional ABI4 extension. Legacy CREATE endpoints retain their exact contract.
// This endpoint reserves all compiled families, including empty/pending ones.
// Preparation completes asynchronously as each source becomes capturable.
#define IOCTL_IC_CREATE_ALL IC_IOCTL(0xA2E)
#define IOCTL_IC_CONTEXT_DEVICES IC_IOCTL(0xA2F)
