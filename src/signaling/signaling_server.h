#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>

struct lws;
struct lws_context;

namespace crystal {

struct SignalingMessage {
    std::string type;
    std::string sdp;
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex = 0;
    std::string to;
    std::string room;
    std::string peerId;
};

class SignalingServer {
public:
    SignalingServer(const std::string& host, uint16_t port);
    ~SignalingServer();

    void start();
    void stop();

    using MessageCallback = std::function<void(const std::string& peerId,
                                               const SignalingMessage& msg)>;
    void onMessage(MessageCallback cb);

    void handleMessage(const std::string& peerId, const std::string& data);
    void sendToPeer(const std::string& peerId, const std::string& data);
    void broadcastToRoom(const std::string& room, const std::string& data,
                         const std::string& excludePeerId);

    void registerPeer(const std::string& peerId, struct lws* wsi, const std::string& room);
    void unregisterPeer(const std::string& peerId);

private:
    std::string host_;
    uint16_t port_;
    lws_context* context_ = nullptr;
    bool running_ = false;

    std::mutex peersMutex_;
    struct PeerInfo {
        struct lws* wsi = nullptr;
        std::string room;
    };
    std::unordered_map<std::string, PeerInfo> peers_;

    MessageCallback messageCallback_;
};

} // namespace crystal
