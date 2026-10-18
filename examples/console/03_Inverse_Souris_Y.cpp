#include "common/Behaviors.h"
// Invert mouse Y; preserve X, buttons, wheels and all other input families.
int wmain(int argc,wchar_t** argv) { return Examples::Run(Examples::Mode::InvertY,argc,argv); }
