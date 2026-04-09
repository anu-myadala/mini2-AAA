// PeerClient.hpp - Thin wrapper around gRPC stubs used by a node to talk
// to its overlay neighbors. A single PeerClient is created per neighbor
// and reused. All calls are blocking unary RPCs (no async APIs).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "Config.hpp"
#include "overlay.grpc.pb.h"

namespace mini2 {

class PeerClient {
public:
    PeerClient(const NodeEntry& entry);

    const std::string& id() const { return entry_.id; }
    const NodeEntry&   entry() const { return entry_; }

    bool forwardQuery(const ForwardQueryRequest& req, ForwardQueryAck* ack);
    bool pushChunk   (const PushChunkRequest&    req, PushChunkAck*    ack);
    bool abort       (const AbortRequest&        req, AbortAck*        ack);
    bool ping        (const PingRequest&         req, PingResponse*    rsp);

private:
    NodeEntry                              entry_;
    std::shared_ptr<grpc::Channel>         channel_;
    std::unique_ptr<Overlay::Stub>         stub_;
};

} // namespace mini2
