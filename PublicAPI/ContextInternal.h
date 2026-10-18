#pragma once
#include "ContextAPI.h"
// Wire-compatibility entry point for the archived regression harnesses only.
// Applications use the single public Create_Context() declaration.
namespace Interceptor { namespace Detail {
struct Preparation_Options {DWORD timeout=0;HANDLE stop_event=nullptr;};
Context_Result Create_Selected_Context(std::uint8_t mask,std::uint8_t controllers,const Preparation_Options& wait={});
}}
