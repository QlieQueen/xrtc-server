#include "base/network.h"

#include <ifaddrs.h>
#include <cstring>

#include <rtc_base/logging.h>

namespace xrtc {

/*
    构造函数做三件事
    1、调用getifaddrs()获取系统所有网络接口的链表
    2、遍历链表，过滤出IPv4非回环地址
    3、为每个有效网卡创建Network对象
*/
NetworkManager::NetworkManager() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return; // 获取网卡信息失败，_networks为空
    }

    // 遍历链表
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue; // 只取IPv4
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;    // 排除回环

        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
        rtc::IPAddress ip_address(addr->sin_addr);

        Network* net = new Network(ifa->ifa_name, ip_address);

        RTC_LOG(LS_INFO) << "[NetworkManager] gathered network interface: " << net->to_string();

        _networks.push_back(net);
    }

    freeifaddrs(ifaddr);
}

NetworkManager::~NetworkManager() {
    _free_networks();
}

Network* NetworkManager::get_first_network() const {
    if (_networks.empty()) {
        return nullptr;
    }

    return _networks[0];
}

void NetworkManager::_free_networks() {
    for (auto net : _networks) {
        delete net;
    }
    _networks.clear();
}


} // namespace xrtc

