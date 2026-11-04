#pragma once
// Private kernel-to-kernel ABI. No user-supplied time or reset operation.
#define TC_VERSION 1u
#define TC_PRODUCTS 4u
#define TC_QUERY 0u
#define TC_COMMIT 1u
#define TC_IOCTL CTL_CODE(0x834Au,0x800,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define TC_STRING_(x) #x
#define TC_STRING(x) TC_STRING_(x)
#define TC_WIDE_(x) L##x
#define TC_WIDE(x) TC_WIDE_(x)
#ifdef TITAN_TRIAL_LAB_SECONDS
#if TITAN_TRIAL_LAB_SECONDS < 1 || TITAN_TRIAL_LAB_SECONDS > 120
#error Laboratory clock duration must be between 1 and 120 seconds.
#endif
#define TC_SECONDS TITAN_TRIAL_LAB_SECONDS
#define TC_NAME L"\\Device\\TitanDemoClockLab" TC_WIDE(TC_STRING(TITAN_TRIAL_LAB_SECONDS))
#define TC_SERVICE_SUFFIX L"Lab" TC_WIDE(TC_STRING(TITAN_TRIAL_LAB_SECONDS))
#else
#define TC_SECONDS 3600u
#define TC_NAME L"\\Device\\TitanDemoClock1"
#define TC_SERVICE_SUFFIX L""
#endif
#define TC_SERVICE_ROOT L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\"
// Each INF owns its service and image path. All services still converge on
// the one fixed resident device, so removing a product cannot erase another
// product's load path or create a second budget during the current boot.
#define TC_SERVICE0 TC_SERVICE_ROOT L"InterceptorDemoClock" TC_SERVICE_SUFFIX
#define TC_SERVICE1 TC_SERVICE_ROOT L"InterceptorKeyboardMouseDemoClock" TC_SERVICE_SUFFIX
#define TC_SERVICE2 TC_SERVICE_ROOT L"InterceptorGamepadControllerDemoClock" TC_SERVICE_SUFFIX
#define TC_SERVICE3 TC_SERVICE_ROOT L"VdeviceDemoClock" TC_SERVICE_SUFFIX
#define TC_DURATION ((ULONGLONG)TC_SECONDS*10000000ull)
// A user-created object-manager link cannot impersonate the clock: only a
// kernel driver can allocate an extension on its DRIVER_OBJECT.
#define TC_EXTENSION_TAG ((PVOID)(ULONG_PTR)0x54434731u)
typedef struct TC_IDENTITY {ULONG size,version;PDRIVER_OBJECT owner;PDEVICE_OBJECT device;} TC_IDENTITY;
typedef struct TC_REQUEST {ULONG size,version,product,operation;} TC_REQUEST;
typedef struct TC_RESULT {ULONG size,version,started,expired;ULONGLONG first,duration;} TC_RESULT;
C_ASSERT(sizeof(TC_REQUEST)==16);
C_ASSERT(sizeof(TC_RESULT)==32);
