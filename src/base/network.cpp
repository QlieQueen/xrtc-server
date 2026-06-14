#include "base/network.h"

#include <ifaddrs.h>
#include <cstring>
#include <arpa/inet.h>

#include <rtc_base/logging.h>

#include "base/conf.h"

namespace xrtc {

/*
    构造函数做三件事
    1、调用getifaddrs()获取系统所有网络接口的链表
    2、遍历链表，过滤出IPv4非回环地址
    3、为每个有效网卡创建Network对象
*/
NetworkManager::NetworkManager(const GeneralConf* conf) {
    // 云服务器场景：手工配置公网 IP，跳过网卡扫描
    if (conf && !conf->netcard.empty() && !conf->ipv4_addr.empty()) {
        struct in_addr addr;
        inet_aton(conf->ipv4_addr.c_str(), &addr);
        Network* net = new Network(conf->netcard, rtc::IPAddress(addr));
        RTC_LOG(LS_INFO) << "[NetworkManager] using configured public IP: " << net->to_string();
        _networks.push_back(net);
        return;
    }

    // 本地场景：自动扫描所有网卡
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (strcmp(ifa->ifa_name, "lo") == 0) continue;

        struct sockaddr_in* saddr = (struct sockaddr_in*)ifa->ifa_addr;
        rtc::IPAddress ip_address(saddr->sin_addr);

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

