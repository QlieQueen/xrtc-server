#ifndef __BASE_NETWORK_H_
#define __BASE_NETWORK_H_

#include <string>
#include <vector>

#include <rtc_base/ip_address.h>

namespace xrtc {

class GeneralConf;

struct Network {
public:
    Network(const std::string& name, const rtc::IPAddress& ip) : 
        _name(name), _ip(ip) { }
    ~Network() = default;

    const std::string name() { return _name; }
    const rtc::IPAddress ip() { return _ip; }

    std::string to_string() {
        return _name + ":" + _ip.ToString();
    }

private:
    std::string _name;  // 网卡名，如“eth0”
    rtc::IPAddress _ip;
};

class NetworkManager {
public:
    NetworkManager(const GeneralConf* conf);
    ~NetworkManager();

    // 获取所有网卡
    const std::vector<Network*>& get_networks() const { return _networks; }

    // 获取第一个可用网卡（ICE至少需要一个地址）
    Network* get_first_network() const;
private:
    std::vector<Network*> _networks;

    void _free_networks();
};

} // namespace xrtc

#endif // __BASE_NETWORK_H_
