#include "signaling/signaling_client.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <cstring>

namespace crystal {

using json = nlohmann::json;

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() {
    disconnect();
}

bool SignalingClient::connect(const std::string& url, uint16_t port) {
    struct lws_context_creation_info ctxInfo;
    memset(&ctxInfo, 0, sizeof(ctxInfo));
    ctxInfo.port = CONTEXT_PORT_NO_LISTEN;
    ctxInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context_ = lws_create_context(&ctxInfo);
    if (!context_) {
        Logger::error("Failed to create WebSocket client context");
        return false;
    }

    struct lws_client_connect_info ccInfo;
    memset(&ccInfo, 0, sizeof(ccInfo));
    ccInfo.context = context_;
    ccInfo.address = url.c_str();
    ccInfo.port = port;
    ccInfo.path = "/";
    ccInfo.host = url.c_str();
    ccInfo.origin = url.c_str();
    ccInfo.protocol = "signaling";

    wsi_ = lws_client_connect_via_info(&ccInfo);
    if (!wsi_) {
        Logger::error("Failed to connect to signaling server");
        lws_context_destroy(context_);
        context_ = nullptr;
        return false;
    }

    connected_ = true;
    serviceThread_ = std::thread(&SignalingClient::serviceThread, this);
    Logger::info("Connected to signaling server at {}:{}", url, port);
    return true;
}

void SignalingClient::disconnect() {
    connected_ = false;
    if (serviceThread_.joinable()) {
        serviceThread_.join();
    }
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    wsi_ = nullptr;
}

void SignalingClient::serviceThread() {
    while (connected_) {
        lws_service(context_, 50);
    }
}

void SignalingClient::joinRoom(const std::string& room) {
    currentRoom_ = room;
    json j;
    j["type"] = "join";
    j["room"] = room;
    handleMessage(j.dump());
}

void SignalingClient::sendOffer(const std::string& sdp,
                                 const std::string& to) {
    json j;
    j["type"] = "offer";
    j["sdp"] = sdp;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::sendAnswer(const std::string& sdp,
                                  const std::string& to) {
    json j;
    j["type"] = "answer";
    j["sdp"] = sdp;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::sendCandidate(const std::string& candidate,
                                     const std::string& sdpMid,
                                     int sdpMLineIndex,
                                     const std::string& to) {
    json j;
    j["type"] = "candidate";
    j["candidate"] = candidate;
    j["sdpMid"] = sdpMid;
    j["sdpMLineIndex"] = sdpMLineIndex;
    j["to"] = to;
    handleMessage(j.dump());
}

void SignalingClient::leaveRoom() {
    json j;
    j["type"] = "leave";
    j["room"] = currentRoom_;
    handleMessage(j.dump());
    currentRoom_.clear();
}

void SignalingClient::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void SignalingClient::handleMessage(const std::string& data) {
    try {
        json j = json::parse(data);
        SignalingMessage msg;
        msg.type = j.value("type", "");
        msg.sdp = j.value("sdp", "");
        msg.candidate = j.value("candidate", "");
        msg.sdpMid = j.value("sdpMid", "");
        msg.sdpMLineIndex = j.value("sdpMLineIndex", 0);
        msg.to = j.value("to", "");
        msg.room = j.value("room", "");
        msg.peerId = j.value("peerId", "");
        if (msg.peerId.empty()) msg.peerId = j.value("from", "");

        if (msg.type == "joined") {
            peerId_ = msg.peerId;
            Logger::info("Joined signaling server, peerId={}", peerId_);
        }

        if (messageCallback_) {
            messageCallback_(msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

} // namespace crystal
