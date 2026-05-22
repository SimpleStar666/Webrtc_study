#pragma once

#include <string>
#include <vector>

namespace crystal {

struct IceConfig {
    std::vector<std::string> stunServers;
    std::vector<std::string> turnServers;
    std::string turnUsername;
    std::string turnPassword;

    static IceConfig defaultConfig() {
        IceConfig cfg;
        cfg.stunServers = {"stun:stun.l.google.com:19302"};
        return cfg;
    }
};

} // namespace crystal
