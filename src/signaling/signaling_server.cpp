#include "signaling/signaling_server.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <thread>
#include <random>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace crystal {

using json = nlohmann::json;

static std::string generatePeerId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    return std::to_string(dist(gen));
}

static SignalingServer* g_server = nullptr;
static std::mutex g_peerMapMutex;
static std::unordered_map<struct lws*, std::string> g_wsiToPeerId;

static int callbackProtocol(struct lws* wsi, enum lws_callback_reasons reason,
                            void* user, void* in, size_t len) {
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED: {
        std::string peerId = generatePeerId();
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            g_wsiToPeerId[wsi] = peerId;
        }
        Logger::info("Signaling: peer connected id={}", peerId);
        break;
    }
    case LWS_CALLBACK_RECEIVE: {
        std::string peerId;
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            auto it = g_wsiToPeerId.find(wsi);
            if (it == g_wsiToPeerId.end()) break;
            peerId = it->second;
        }
        if (!g_server) break;
        std::string data(static_cast<const char*>(in), len);
        Logger::debug("Signaling: received from {}: {} bytes", peerId, len);
        g_server->handleMessage(peerId, data);
        break;
    }
    case LWS_CALLBACK_CLOSED: {
        std::string peerId;
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            auto it = g_wsiToPeerId.find(wsi);
            if (it == g_wsiToPeerId.end()) break;
            peerId = it->second;
            g_wsiToPeerId.erase(it);
        }
        Logger::info("Signaling: peer disconnected id={}", peerId);
        if (g_server) {
            g_server->unregisterPeer(peerId);
            g_server->handleMessage(peerId, "{\"type\":\"leave\"}");
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"signaling", callbackProtocol, 0, 4096, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0}
};

SignalingServer::SignalingServer(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

SignalingServer::~SignalingServer() {
    stop();
}

void SignalingServer::start() {
    g_server = this;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port_;
    info.iface = host_.c_str();
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;

    context_ = lws_create_context(&info);
    if (!context_) {
        Logger::error("Failed to create libwebsocket context");
        return;
    }

    Logger::info("Signaling server started on {}:{}", host_, port_);
    running_ = true;

    while (running_) {
        lws_service(context_, 50);
    }

    lws_context_destroy(context_);
    context_ = nullptr;
    g_server = nullptr;
}

void SignalingServer::stop() {
    running_ = false;
}

void SignalingServer::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

void SignalingServer::registerPeer(const std::string& peerId, struct lws* wsi, const std::string& room) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_[peerId].wsi = wsi;
    peers_[peerId].room = room;
}

void SignalingServer::unregisterPeer(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_.erase(peerId);
}

void SignalingServer::handleMessage(const std::string& peerId,
                                     const std::string& data) {
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
        msg.peerId = peerId;

        if (msg.type == "join") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            peers_[peerId].room = msg.room;
            Logger::info("Peer {} joined room {}", peerId, msg.room);

            json notify;
            notify["type"] = "peer_joined";
            notify["peerId"] = peerId;
            broadcastToRoom(msg.room, notify.dump(), peerId);

            json welcome;
            welcome["type"] = "joined";
            welcome["peerId"] = peerId;
            sendToPeer(peerId, welcome.dump());
        } else if (msg.type == "offer" || msg.type == "answer" ||
                   msg.type == "candidate") {
            if (!msg.to.empty()) {
                json forward = j;
                forward["from"] = peerId;
                sendToPeer(msg.to, forward.dump());
            }
        } else if (msg.type == "leave") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            std::string room = peers_[peerId].room;
            peers_.erase(peerId);

            json notify;
            notify["type"] = "peer_left";
            notify["peerId"] = peerId;
            broadcastToRoom(room, notify.dump(), "");
        }

        if (messageCallback_) {
            messageCallback_(peerId, msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

void SignalingServer::sendToPeer(const std::string& peerId,
                                  const std::string& data) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end() && it->second.wsi) {
        std::vector<uint8_t> buf(LWS_PRE + data.size());
        std::memcpy(buf.data() + LWS_PRE, data.data(), data.size());
        lws_write(it->second.wsi, buf.data() + LWS_PRE, data.size(),
                  LWS_WRITE_TEXT);
    }
}

void SignalingServer::broadcastToRoom(const std::string& room,
                                       const std::string& data,
                                       const std::string& excludePeerId) {
    for (const auto& [id, info] : peers_) {
        if (id != excludePeerId && info.room == room) {
            sendToPeer(id, data);
        }
    }
}

} // namespace crystal
