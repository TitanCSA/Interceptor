#pragma once
#include "ContextAPI.h"
#include <system_error>

namespace Interceptor::Detail {
// Shared with the real processor and its fault-injection tests. A partial
// send is never replayed: the public owning wrapper closes capture on error.
template<class T,class Read,class Send>
std::size_t ProcessBatch(std::vector<T>& inputs,Read&& read,Send&& send,bool (*handler)(void*,T&),void* user) {
    read(inputs);
    const auto received=inputs.size();std::size_t kept=0;
    for(std::size_t i=0;i<received;++i) {
        if(handler && !handler(user,inputs[i]))continue;
        if(kept!=i)inputs[kept]=inputs[i];
        ++kept;
    }
    if(kept) {
        const auto result=send(Detail::Input_View<const T>(inputs.data(),kept));
        if(result.error || result.accepted!=kept)
            throw std::system_error(static_cast<int>(result.error?result.error:ERROR_WRITE_FAULT),std::system_category(),"Forward processed input");
    }
    return received;
}
}
