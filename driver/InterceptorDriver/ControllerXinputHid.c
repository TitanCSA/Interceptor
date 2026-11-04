// Instantiate the shared collection engine BELOW xinputhid.sys, so both its
// native XInput path and DirectInput view consume only the delivered reports.
#include "ControllerXinputHid.h"
#define IC_HID_BACKEND_TYPE IC_TYPE_CONTROLLER_XINPUTHID
#define IC_HID_BACKEND_GUID IC_XINPUTHID_INTERFACE_GUID
#define IC_HID_BACKEND_SOURCE IOCTL_IC_XINPUTHID_SOURCE
#define IcHidInitialize IcXinputHidInitialize
#define IcHidAdd IcXinputHidAdd
#define IcHidCountLocked IcXinputHidCountLocked
#define IcHidArmLocked IcXinputHidArmLocked
#define IcHidRefreshCaptureLocked IcXinputHidRefreshCaptureLocked
#define IcHidSnapshotLocked IcXinputHidSnapshotLocked
#define IcHidCaptureLocked IcXinputHidCaptureLocked
#define IcHidResetLocked IcXinputHidResetLocked
#define IcHidGetLocked IcXinputHidGetLocked
#define IcHidGetSelectedLocked IcXinputHidGetSelectedLocked
#define IcHidSendLocked IcXinputHidSendLocked
#define IcHidDescriptorLocked IcXinputHidDescriptorLocked
#define IcHidQueuedLocked IcXinputHidQueuedLocked
#define IcHidQueryLocked IcXinputHidQueryLocked
#define IcHidMonitorChangedLocked IcXinputHidMonitorChangedLocked
#define IcHidMonitorDeviceLocked IcXinputHidMonitorDeviceLocked
#include "ControllerHidEngine.h"
