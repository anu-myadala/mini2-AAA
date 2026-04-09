// PeerClient.cpp
#include "PeerClient.hpp"

#include <chrono>

namespace mini2 {

namespace {
constexpr int kCallTimeoutMs = 5000;

std::shared_ptr<grpc::Channel> makeChannel(const NodeEntry& e) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(64 * 1024 * 1024);
    args.SetMaxSendMessageSize(64 * 1024 * 1024);
    return grpc::CreateCustomChannel(
        e.host + ":" + std::to_string(e.port),
        grpc::InsecureChannelCredentials(),
        args);
}

void setDeadline(grpc::ClientContext& ctx, int ms) {
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::milliseconds(ms));
}
} // namespace

PeerClient::PeerClient(const NodeEntry& entry) : entry_(entry) {
    channel_ = makeChannel(entry);
    stub_    = Overlay::NewStub(channel_);
}

bool PeerClient::forwardQuery(const ForwardQueryRequest& req, ForwardQueryAck* ack) {
    grpc::ClientContext ctx; setDeadline(ctx, kCallTimeoutMs);
    return stub_->ForwardQuery(&ctx, req, ack).ok();
}

bool PeerClient::pushChunk(const PushChunkRequest& req, PushChunkAck* ack) {
    grpc::ClientContext ctx; setDeadline(ctx, kCallTimeoutMs * 2);
    return stub_->PushChunk(&ctx, req, ack).ok();
}

bool PeerClient::abort(const AbortRequest& req, AbortAck* ack) {
    grpc::ClientContext ctx; setDeadline(ctx, kCallTimeoutMs);
    return stub_->Abort(&ctx, req, ack).ok();
}

bool PeerClient::ping(const PingRequest& req, PingResponse* rsp) {
    grpc::ClientContext ctx; setDeadline(ctx, 1000);
    return stub_->Ping(&ctx, req, rsp).ok();
}

} // namespace mini2
