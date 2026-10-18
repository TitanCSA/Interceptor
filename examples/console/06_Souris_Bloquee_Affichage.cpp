#include "common/Behaviors.h"
// Display mouse input without forwarding it; other families pass through.
int wmain(int argc,wchar_t** argv) { return Examples::Run(Examples::Mode::MonitorMouse,argc,argv); }
