#ifdef _WIN32
#include <cstring>
// clang-format off: delayimp.h depends on Windows type declarations.
#include <windows.h>
#include <delayimp.h>
// clang-format on
// Resolve Node-API from the current host, including renamed Node executables.
static FARPROC WINAPI Host(unsigned event, PDelayLoadInfo info) {
    if (event == dliNotePreLoadLibrary && std::strcmp(info->szDll, "node.exe") == 0)
        return reinterpret_cast<FARPROC>(GetModuleHandleW(nullptr));
    return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = Host;
#endif
