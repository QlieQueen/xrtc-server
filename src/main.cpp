#include <iostream>

#include "base/conf.h"

xrtc::GeneralConf* g_conf = nullptr;

int init_general_conf(const char* filename) {
    if (!filename) {
        fprintf(stderr, "filename is nullptr\n");
        return -1;
    }

    g_conf = new xrtc::GeneralConf();



    int ret = load_general_conf(filename, g_conf);

    
    return 0;
}

int main() {

    if (!init_general_conf("./conf/general.yaml")) {

    }


    return 0;
}
