#include "common/Behaviors.h"
// Swap keyboard H/J; the runner forwards all untouched input families.
int wmain(int argc,wchar_t** argv) { return Examples::Run(Examples::Mode::SwapHJ,argc,argv); }
