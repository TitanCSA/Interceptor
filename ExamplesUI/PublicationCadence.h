#pragma once
#include <cstdint>
#include <limits>

namespace Viewer {
// Presentation cadence only. Captured reports are forwarded before this is
// consulted. The capture wait supplies the one-shot idle flush; no timer or
// polling thread runs when there is nothing to publish.
class PublicationCadence {
    static constexpr std::uint32_t Interval=33;
    std::uint64_t last_=0;
    bool pending_=false;
public:
    void Changed() noexcept {pending_=true;}
    void Committed(std::uint64_t now) noexcept {last_=now;pending_=false;}
    std::uint32_t Wait(std::uint64_t now) const noexcept {
        if(!pending_)return std::numeric_limits<std::uint32_t>::max();
        const auto elapsed=now-last_;
        return elapsed>=Interval?0:Interval-static_cast<std::uint32_t>(elapsed);
    }
    bool Due(std::uint64_t now) const noexcept {return Wait(now)==0;}
};
}
