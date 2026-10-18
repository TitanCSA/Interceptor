#include "Interceptor.h"
#include <iostream>
#include <system_error>

int wmain() {
    using namespace Interceptor;
    try {
        const auto init=Init_AllDevices();
        if(!init.ready)return static_cast<int>(init.error?init.error:ERROR_SUCCESS_REBOOT_REQUIRED);
        for(;;) {
            // The library opens/reuses its session automatically. The batch
            // borrows reusable storage until the end of this iteration.
            auto inputs=Get_Input_With_Timeout(INFINITE,nullptr);
            for(auto& event:inputs) {
                if(auto* key=event.Get<Keyboard_Input>()) {
                    if(!(key->stroke.flags&(KeyE0|KeyE1)) && key->stroke.make_code==0x23)
                        key->stroke.make_code=0x24;
                }
            }
            const auto sent=Send_Input(inputs);
            if(!sent)throw std::system_error(static_cast<int>(sent.error),std::system_category(),"Forward input batch");
        }
    }catch(const std::system_error& e) {
        if(e.code().value()==ERROR_OPERATION_ABORTED)return 0;
        std::cerr<<e.what()<<'\n';return 1;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
