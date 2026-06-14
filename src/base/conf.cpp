#include <yaml-cpp/yaml.h>

#include "base/conf.h"

namespace xrtc {

int load_general_conf(const char *filename, GeneralConf* conf) {
    if (!filename || !conf) {
        fprintf(stderr, "filename or conf is nullptr\n");
        return -1;
    }

    conf->log_dir = "./log";
    conf->log_name = "undefined";
    conf->log_level = "info";
    conf->log_to_stderr = false;

    try {
        YAML::Node config = YAML::LoadFile(filename);

        conf->log_dir = config["log"]["log_dir"].as<std::string>();
        conf->log_name = config["log"]["log_name"].as<std::string>();
        conf->log_level = config["log"]["log_level"].as<std::string>();
        conf->log_to_stderr = config["log"]["log_to_stderr"].as<bool>();

        // ice
        conf->netcard = config["ice"]["netcard"].as<std::string>();
        conf->ipv4_addr = config["ice"]["ipv4_addr"].as<std::string>();
        conf->ice_min_port = config["ice"]["min_port"].as<int>();
        conf->ice_max_port = config["ice"]["max_port"].as<int>();
    } catch (YAML::Exception &e) {
        fprintf(stderr, "catch a YAML::Exception, line: %d, colum: %d, err: %s\n",
            e.mark.line, e.mark.column, e.msg.c_str());
        return -1;
    } catch (std::exception &e) {
        fprintf(stderr, "catch a std::exception, err: %s\n", e.what());
        return -1;
    }

    return 0;
}


} // namespace xrtc
