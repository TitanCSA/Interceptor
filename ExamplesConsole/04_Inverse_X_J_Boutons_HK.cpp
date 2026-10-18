#include "Behaviors.h"
// J and X1/X2 hold H+K without premature releases; mouse X is inverted.
int wmain(int argc,wchar_t** argv) { return Examples::Run(Examples::Mode::InvertXAndHK,argc,argv); }
