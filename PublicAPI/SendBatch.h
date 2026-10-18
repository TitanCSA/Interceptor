#pragma once
#include "Interceptor.h"
#include <algorithm>
#include <utility>

namespace Interceptor::Detail {
// A failed factory destroys its partial resources before returning. The cache
// only becomes ready after the complete source set has been prepared.
template<class Resources> class SourceCache {
    Resources resources_;
    bool ready_=false;
public:
    template<class Factory> void Prepare(Factory&& factory) {
        if(ready_)return;
        auto replacement=factory();
        resources_=std::move(replacement);ready_=true;
    }
    void Reset() noexcept {resources_.clear();ready_=false;}
};
inline bool SourceUnavailable(DWORD error) noexcept {
    return error==ERROR_DEVICE_NOT_CONNECTED || error==ERROR_NOT_READY;
}
// send(offset,count) reports an accepted prefix. Refresh is allowed once per
// public call, only for a remaining suffix; a committed input is never resent.
template<class Submit,class Refresh>
Send_Result SendBatches(std::size_t count,Submit&& send,Refresh&& refresh,bool canRefresh) {
    std::size_t accepted=0;bool refreshed=false;
    while(accepted<count) {
        const auto n=std::min<std::size_t>(IC_MAX_SEND,count-accepted);
        const auto result=send(accepted,n);
        if(result.accepted>n)return {accepted,ERROR_INVALID_DATA};
        accepted+=result.accepted;
        if(result.error) {
            if(canRefresh && !refreshed && accepted<count && SourceUnavailable(result.error)) {
                refreshed=true;
                const DWORD error=refresh();
                if(error)return {accepted,error};
                continue;
            }
            return {accepted,result.error};
        }
        if(result.accepted!=n)return {accepted,ERROR_INVALID_DATA};
    }
    return {accepted,ERROR_SUCCESS};
}
}
