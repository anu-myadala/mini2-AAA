// server_main.cpp - mini2 node entry point.
//
// Usage:  node_server --id <NODE_ID> --config <topology.conf>
//
// Note: NOTHING is hard-coded. The id determines which row of the config
// table this process embodies.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Config.hpp"
#include "NodeServer.hpp"

using namespace mini2;

namespace {
void usage() {
    std::cerr << "usage: node_server --id <NODE_ID> --config <topology.conf>\n";
}
} // namespace

int main(int argc, char** argv) {
    std::string id;
    std::string cfg_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            id = argv[++i];
        } else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            cfg_path = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (id.empty() || cfg_path.empty()) { usage(); return 2; }

    try {
        Config cfg = Config::load(cfg_path);
        if (cfg.nodes.find(id) == cfg.nodes.end()) {
            std::cerr << "node id " << id << " not present in config\n";
            return 2;
        }
        NodeServer srv(id, std::move(cfg));
        srv.run();
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
