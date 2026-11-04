#pragma once
#include "InterceptorProtocol.h"
static __inline int IcValidKeyboard(const IC_KEYBOARD_INPUT* k) {
    return !k->reserved && !k->padding && !k->stroke.reserved && k->stroke.make_code<=127 &&
        !(k->stroke.flags&~7u) && (k->stroke.flags&6u)!=6u;
}
static __inline int IcValidMouse(const IC_MOUSE_INPUT* m) {
    return !m->reserved && !(m->stroke.flags&~0x0Bu) && !(m->stroke.button_flags&~0x0FFFu) &&
        (m->stroke.button_flags&0x0C00u)!=0x0C00u;
}
