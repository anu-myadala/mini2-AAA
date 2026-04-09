// Config.hpp - Topology configuration for a mini2 node.
//
// We keep the format intentionally primitive (line-oriented key=value) to
// avoid dragging in a JSON/YAML dependency. See config/topology.conf.
//
// Nothing about identity/role/ports is hard-coded in source: every node is
// started with --id <X> --config <path> and pulls everything else from here.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mini2 {

struct NodeEntry {
    std::string id;
    std::string host;
    uint16_t    port = 0;
    std::string lang;      // "cpp" | "python"
    bool        is_root = false;
};

struct Config {
    // All nodes in the overlay, keyed by id.
    std::unordered_map<std::string, NodeEntry> nodes;

    // Adjacency list (undirected). peers[id] = set of neighbor ids.
    std::unordered_map<std::string, std::unordered_set<std::string>> peers;

    // Partition csv path for each node id.
    std::unordered_map<std::string, std::string> partitions;

    // Runtime parameters (all tunables live here, not in code).
    std::unordered_map<std::string, std::string> params;

    // Parse a topology.conf file. Throws std::runtime_error on failure.
    static Config load(const std::string& path);

    // Convenience accessors with defaults.
    int    paramInt   (const std::string& key, int    def) const;
    double paramDouble(const std::string& key, double def) const;

    const NodeEntry& node(const std::string& id) const;
    const std::unordered_set<std::string>& neighborsOf(const std::string& id) const;
    std::string partitionPath(const std::string& id) const;

    // Root (portal) node id. Throws if no root is marked.
    std::string rootId() const;

    std::vector<std::string> allNodeIds() const;
};

} // namespace mini2
