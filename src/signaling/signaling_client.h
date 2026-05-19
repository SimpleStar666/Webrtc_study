#pragma once

#include "signaling/signaling_server.h"
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

struct lws;
struct lws_context;

namespace crystal {

class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    bool connect(const std::string& url, uint16_t port);
    void disconnect();

    void joinRoom(const std::string& room);
    void sendOffer(const std::string& sdp, const std::string& to);
    void sendAnswer(const std::string& sdp, const std::string& to);
    void sendCandidate(const std::string& candidate,
                       const std::string& sdpMid,
                       int sdpMLineIndex,
                       const std::string& to);
    void leaveRoom();

    using MessageCallback = std::function<void(const SignalingMessage& msg)>;
    void onMessage(MessageCallback cb);

    const std::string& peerId() const { return peerId_; }

private:
    void serviceThread();
    void handleMessage(const std::string& data);

    lws_context* context_ = nullptr;
    lws* wsi_ = nullptr;
    std::atomic<bool> connected_{false};
    std::thread serviceThread_;
    std::string peerId_;
    std::string currentRoom_;
    MessageCallback messageCallback_;

    std::mutex sendMutex_;
    std::vector<std::string> pendingSends_;
};

} // namespace crystal
