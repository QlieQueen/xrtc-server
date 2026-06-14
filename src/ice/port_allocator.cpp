#include "ice/port_allocator.h"

#include "base/conf.h"

namespace xrtc {

PortAllocator::PortAllocator(const GeneralConf* conf) : 
    _network_manager(new NetworkManager(conf))
{
}

PortAllocator::~PortAllocator() = default;

void PortAllocator::set_port_range(int min_port, int max_port) {
    if (min_port > 0) {
        _min_port = min_port;
    }

    if (max_port > 0) {
        _max_port = max_port;
    }
}

const std::vector<Network*>& PortAllocator::get_networks() {
    return _network_manager->get_networks();
}

} // namespace xrtc