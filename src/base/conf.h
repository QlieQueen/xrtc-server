#ifndef XRTC_SERVER_SRC_BASE_CONF_H_
#define XRTC_SERVER_SRC_BASE_CONF_H_

#include <string>

namespace xrtc {

struct GeneralConf {
    std::string log_dir;
    std::string log_name;
    std::string log_level;
    bool log_to_stderr;

    // ice
    int ice_min_port = 0;
    int ice_max_port = 0;

    std::string ipv4_addr;
    std::string netcard;
};

int load_general_conf(const char* filename, GeneralConf* conf);

} // namespace xrtc



#endif // XRTC_SERVER_SRC_BASE_CONF_H_