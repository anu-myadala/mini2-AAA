// Config.cpp - parser for the line-oriented topology.conf format.
#include "Config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mini2 {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> splitWhitespace(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

bool parseKv(const std::string& tok, std::string* k, std::string* v) {
    auto eq = tok.find('=');
    if (eq == std::string::npos) return false;
    *k = tok.substr(0, eq);
    *v = tok.substr(eq + 1);
    return true;
}

} // namespace

Config Config::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Config: cannot open " + path);

    Config cfg;
    std::string raw_line;
    int lineno = 0;
    while (std::getline(in, raw_line)) {
        ++lineno;
        // strip comments
        auto hash = raw_line.find('#');
        if (hash != std::string::npos) raw_line.erase(hash);
        std::string line = trim(raw_line);
        if (line.empty()) continue;

        auto toks = splitWhitespace(line);
        const std::string& kw = toks[0];

        if (kw == "node") {
            if (toks.size() < 2) {
                throw std::runtime_error("Config: bad node line " + std::to_string(lineno));
            }
            NodeEntry e;
            e.id = toks[1];
            for (std::size_t i = 2; i < toks.size(); ++i) {
                std::string k, v;
                if (!parseKv(toks[i], &k, &v)) continue;
                if      (k == "host") e.host = v;
                else if (k == "port") e.port = static_cast<uint16_t>(std::atoi(v.c_str()));
                else if (k == "lang") e.lang = v;
                else if (k == "root") e.is_root = (v == "true" || v == "1" || v == "yes");
            }
            if (e.host.empty() || e.port == 0) {
                throw std::runtime_error("Config: node " + e.id + " missing host/port");
            }
            cfg.nodes[e.id] = e;
            cfg.peers[e.id]; // ensure key exists

        } else if (kw == "edge") {
            if (toks.size() != 3) {
                throw std::runtime_error("Config: bad edge line " + std::to_string(lineno));
            }
            cfg.peers[toks[1]].insert(toks[2]);
            cfg.peers[toks[2]].insert(toks[1]);

        } else if (kw == "partition") {
            if (toks.size() != 3) {
                throw std::runtime_error("Config: bad partition line " + std::to_string(lineno));
            }
            cfg.partitions[toks[1]] = toks[2];

        } else if (kw == "param") {
            if (toks.size() != 3) {
                throw std::runtime_error("Config: bad param line " + std::to_string(lineno));
            }
            cfg.params[toks[1]] = toks[2];

        } else {
            throw std::runtime_error("Config: unknown directive '" + kw +
                                     "' on line " + std::to_string(lineno));
        }
    }
    return cfg;
}

int Config::paramInt(const std::string& key, int def) const {
    auto it = params.find(key);
    if (it == params.end()) return def;
    return std::atoi(it->second.c_str());
}

double Config::paramDouble(const std::string& key, double def) const {
    auto it = params.find(key);
    if (it == params.end()) return def;
    return std::atof(it->second.c_str());
}

const NodeEntry& Config::node(const std::string& id) const {
    auto it = nodes.find(id);
    if (it == nodes.end()) throw std::runtime_error("Config: unknown node id " + id);
    return it->second;
}

const std::unordered_set<std::string>& Config::neighborsOf(const std::string& id) const {
    static const std::unordered_set<std::string> empty;
    auto it = peers.find(id);
    if (it == peers.end()) return empty;
    return it->second;
}

std::string Config::partitionPath(const std::string& id) const {
    auto it = partitions.find(id);
    if (it == partitions.end()) return {};
    return it->second;
}

std::string Config::rootId() const {
    for (const auto& kv : nodes) {
        if (kv.second.is_root) return kv.first;
    }
    throw std::runtime_error("Config: no node marked root=true");
}

std::vector<std::string> Config::allNodeIds() const {
    std::vector<std::string> v;
    v.reserve(nodes.size());
    for (const auto& kv : nodes) v.push_back(kv.first);
    return v;
}

} // namespace mini2
