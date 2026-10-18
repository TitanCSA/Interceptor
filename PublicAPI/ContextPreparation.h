#pragma once
#include <Windows.h>
#include <algorithm>
#include <system_error>

namespace Interceptor { namespace Detail {
// Shared by CREATE and host-safe fault tests. Never restart a revoked capture:
// this loop is used only before a context has been successfully acquired.
template<class Attempt,class Clock,class Pause>
void PrepareContext(Attempt attempt,Clock now,Pause pause,DWORD timeout) {
    const auto started=now();
    for(;;) {
        // A zero-duration pause checks cancellation before the first attempt.
        pause(0);
        try {attempt();return;}
        catch(const std::system_error& error) {
            if(error.code().value()!=ERROR_NOT_READY || !timeout)throw;
            const auto elapsed=now()-started;
            if(elapsed>=timeout)throw std::system_error(ERROR_TIMEOUT,std::system_category(),"Device present but capture preparation timed out");
            pause(static_cast<DWORD>(std::min<ULONGLONG>(100,timeout-elapsed)));
        }
    }
}
}}
