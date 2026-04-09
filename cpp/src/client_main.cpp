// client_main.cpp - mini2 portal client.
//
// Submits a query to node A (portal), then drains chunks until final.
// Honors the portal's dynamic chunk-size suggestion. Demonstrates cancel
// via Ctrl-C (SIGINT) which sends CancelQuery before exiting.
//
// Usage examples:
//   portal_client --config config/topology.conf
//   portal_client --config config/topology.conf --borough BROOKLYN --max 100000
//   portal_client --config config/topology.conf --from 20200101 --to 20201231
//   portal_client --config config/topology.conf --geo 40.7,40.8,-74.05,-73.95
//   portal_client --config config/topology.conf --portal A --slow

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "Config.hpp"
#include "overlay.grpc.pb.h"

using namespace mini2;

namespace {

std::atomic<bool> g_interrupted{false};
std::string       g_request_id;
std::unique_ptr<Portal::Stub> g_stub;

void handleSig(int) { g_interrupted.store(true); }

void usage() {
    std::cerr <<
        "usage: portal_client --config <topology.conf> [opts]\n"
        "  --portal <id>            which node to talk to (default: root)\n"
        "  --borough <NAME>         MANHATTAN|BROOKLYN|QUEENS|BRONX|STATEN ISLAND\n"
        "  --complaint <STR>        exact complaint type match\n"
        "  --from <YYYYMMDD>        date range lower bound (inclusive)\n"
        "  --to <YYYYMMDD>          date range upper bound (inclusive)\n"
        "  --geo <minLat,maxLat,minLon,maxLon>\n"
        "  --max <N>                stop after N rows\n"
        "  --slow                   sleep between fetches (demo backpressure)\n"
        "  --client-id <STR>        identify yourself in the portal logs\n";
}

BoroughEnum parseBorough(const std::string& s) {
    if (s == "MANHATTAN")     return B_MANHATTAN;
    if (s == "BROOKLYN")      return B_BROOKLYN;
    if (s == "QUEENS")        return B_QUEENS;
    if (s == "BRONX")         return B_BRONX;
    if (s == "STATEN ISLAND") return B_STATEN_ISLAND;
    return B_UNSPECIFIED;
}

bool parseGeo(const std::string& s, QuerySpec& q) {
    double a, b, c, d;
    if (std::sscanf(s.c_str(), "%lf,%lf,%lf,%lf", &a, &b, &c, &d) != 4) return false;
    q.set_use_geo_box(true);
    q.set_min_lat(a); q.set_max_lat(b);
    q.set_min_lon(c); q.set_max_lon(d);
    return true;
}

const char* boroughString(BoroughEnum b) {
    switch (b) {
        case B_MANHATTAN:     return "MANHATTAN";
        case B_BROOKLYN:      return "BROOKLYN";
        case B_QUEENS:        return "QUEENS";
        case B_BRONX:         return "BRONX";
        case B_STATEN_ISLAND: return "STATEN ISLAND";
        default:              return "UNSPECIFIED";
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string cfg_path;
    std::string portal_id;
    std::string client_id = "cli";
    bool slow = false;
    QuerySpec spec;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* /*flag*/) -> std::string {
            if (i + 1 >= argc) { usage(); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--config")    cfg_path = need("--config");
        else if (a == "--portal")    portal_id = need("--portal");
        else if (a == "--client-id") client_id = need("--client-id");
        else if (a == "--borough")   spec.set_borough(parseBorough(need("--borough")));
        else if (a == "--complaint") spec.set_complaint_type(need("--complaint"));
        else if (a == "--from")      spec.set_date_from(static_cast<uint32_t>(std::atoi(need("--from").c_str())));
        else if (a == "--to")        spec.set_date_to  (static_cast<uint32_t>(std::atoi(need("--to").c_str())));
        else if (a == "--max")       spec.set_max_rows (static_cast<uint32_t>(std::atoi(need("--max").c_str())));
        else if (a == "--geo")       parseGeo(need("--geo"), spec);
        else if (a == "--slow")      slow = true;
        else { usage(); return 2; }
    }
    if (cfg_path.empty()) { usage(); return 2; }

    Config cfg;
    try { cfg = Config::load(cfg_path); }
    catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }

    if (portal_id.empty()) portal_id = cfg.rootId();
    const auto& pe = cfg.node(portal_id);
    std::string addr = pe.host + ":" + std::to_string(pe.port);
    std::cerr << "[client] portal=" << portal_id << " addr=" << addr << "\n";

    auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
    g_stub = Portal::NewStub(channel);

    std::signal(SIGINT, handleSig);

    SubmitRequest sreq;
    sreq.set_client_id(client_id);
    *sreq.mutable_spec() = spec;
    SubmitAck sack;
    grpc::ClientContext sctx;
    auto status = g_stub->SubmitQuery(&sctx, sreq, &sack);
    if (!status.ok() || !sack.accepted()) {
        std::cerr << "[client] submit failed: "
                  << (status.ok() ? sack.error() : status.error_message()) << "\n";
        return 1;
    }
    g_request_id = sack.request_id();
    std::cerr << "[client] request_id=" << g_request_id << "\n";

    std::uint64_t total = 0;
    std::uint32_t want  = 0; // 0 lets the portal pick
    auto t0 = std::chrono::steady_clock::now();

    while (true) {
        if (g_interrupted.load()) {
            std::cerr << "[client] SIGINT - sending CancelQuery\n";
            CancelRequest cr; cr.set_request_id(g_request_id); cr.set_reason("user");
            CancelAck     ca;
            grpc::ClientContext cctx;
            (void)g_stub->CancelQuery(&cctx, cr, &ca);
            break;
        }

        FetchRequest fr;
        fr.set_request_id(g_request_id);
        fr.set_max_rows(want);
        fr.set_block(true);
        ChunkResponse cr;
        grpc::ClientContext fctx;
        auto fs = g_stub->FetchChunk(&fctx, fr, &cr);
        if (!fs.ok()) {
            std::cerr << "[client] fetch failed: " << fs.error_message() << "\n";
            break;
        }

        for (const auto& r : cr.rows()) {
            std::cout << r.unique_key() << ','
                      << r.created_ymd() << ','
                      << boroughString(r.borough()) << ','
                      << r.latitude() << ','
                      << r.longitude() << ','
                      << r.complaint_type() << '\n';
        }
        total += cr.rows_size();
        want = cr.suggested_next_max();

        if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(750));

        if (cr.final()) {
            std::cerr << "[client] done. rows=" << total
                      << "  pending_producers=" << cr.pending_producers()
                      << "  canceled=" << (cr.canceled() ? "yes" : "no") << "\n";
            break;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cerr << "[client] elapsed_ms=" << ms << " rows=" << total << "\n";
    return 0;
}
