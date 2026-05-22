// ============================================================================
// signaling_client.cpp - WebSocket 信令客户端实现
// ============================================================================
//
// 本文件实现了 SignalingClient 类，提供与信令服务器通信的客户端功能。
//
// 【libwebsockets 客户端使用方式】
// 1. 创建客户端上下文：lws_create_context
//    - 设置 port 为 CONTEXT_PORT_NO_LISTEN（客户端不需要监听端口）
//    - 设置 LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT 以支持 WSS 连接
//
// 2. 配置连接参数：lws_client_connect_info
//    - context: 上下文指针
//    - address: 服务器地址
//    - port: 服务器端口
//    - path: WebSocket 路径（通常为 "/"）
//    - host/origin: HTTP 头字段
//    - protocol: 子协议名称（需与服务端一致，本项目为 "signaling"）
//
// 3. 发起连接：lws_client_connect_via_info
//    - 返回 lws* 指针表示连接实例
//
// 4. 运行事件循环：lws_service
//    - 在独立线程中循环调用，处理所有 WebSocket 事件
//
// ============================================================================

#include "signaling/signaling_client.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <libwebsockets.h>
#include <cstring>

namespace crystal {

using json = nlohmann::json;

// 默认构造函数
SignalingClient::SignalingClient() = default;

// 析构函数，确保断开连接
SignalingClient::~SignalingClient() {
    disconnect();
}

// 连接到信令服务器
// 参数：
//   url  - 服务器地址（如 "127.0.0.1"）
//   port - 服务器端口号
// 返回值：true 连接成功，false 连接失败
bool SignalingClient::connect(const std::string& url, uint16_t port) {
    // 创建 libwebsockets 客户端上下文
    struct lws_context_creation_info ctxInfo;
    memset(&ctxInfo, 0, sizeof(ctxInfo));
    // 客户端模式：不需要监听端口
    ctxInfo.port = CONTEXT_PORT_NO_LISTEN;
    // 启用 SSL 全局初始化，支持 wss:// 安全连接
    ctxInfo.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context_ = lws_create_context(&ctxInfo);
    if (!context_) {
        Logger::error("Failed to create WebSocket client context");
        return false;
    }

    // 配置 WebSocket 客户端连接参数
    struct lws_client_connect_info ccInfo;
    memset(&ccInfo, 0, sizeof(ccInfo));
    ccInfo.context = context_;           // 上下文指针
    ccInfo.address = url.c_str();        // 服务器地址
    ccInfo.port = port;                  // 服务器端口
    ccInfo.path = "/";                   // WebSocket 路径
    ccInfo.host = url.c_str();           // HTTP Host 头
    ccInfo.origin = url.c_str();         // HTTP Origin 头
    ccInfo.protocol = "signaling";       // WebSocket 子协议（需与服务端一致）

    // 发起 WebSocket 连接
    wsi_ = lws_client_connect_via_info(&ccInfo);
    if (!wsi_) {
        Logger::error("Failed to connect to signaling server");
        lws_context_destroy(context_);
        context_ = nullptr;
        return false;
    }

    connected_ = true;
    // 启动事件循环线程
    // 在独立线程中运行 lws_service，避免阻塞主线程
    serviceThread_ = std::thread(&SignalingClient::serviceThread, this);
    Logger::info("Connected to signaling server at {}:{}", url, port);
    return true;
}

// 断开与信令服务器的连接
void SignalingClient::disconnect() {
    connected_ = false; // 设置标志，使事件循环线程退出
    // 等待事件循环线程结束
    if (serviceThread_.joinable()) {
        serviceThread_.join();
    }
    // 销毁 libwebsockets 上下文
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    wsi_ = nullptr;
}

// 事件循环线程函数
// 在独立线程中持续运行，处理 WebSocket 事件
// lws_service 每次调用处理一批事件，超时 50ms
void SignalingClient::serviceThread() {
    while (connected_) {
        lws_service(context_, 50);
    }
}

// 加入房间
// 向信令服务器发送 join 消息
void SignalingClient::joinRoom(const std::string& room) {
    currentRoom_ = room; // 记录当前房间
    json j;
    j["type"] = "join";
    j["room"] = room;
    handleMessage(j.dump());
}

// 发送 SDP Offer 给指定对等端
// 在 WebRTC 连接建立流程中，呼叫方创建 Offer 后通过此方法发送
void SignalingClient::sendOffer(const std::string& sdp,
                                 const std::string& to) {
    json j;
    j["type"] = "offer";
    j["sdp"] = sdp;   // SDP Offer 字符串，包含媒体格式和传输参数
    j["to"] = to;     // 目标对等端 ID
    handleMessage(j.dump());
}

// 发送 SDP Answer 给指定对等端
// 被叫方在收到 Offer 并生成 Answer 后通过此方法发送
void SignalingClient::sendAnswer(const std::string& sdp,
                                  const std::string& to) {
    json j;
    j["type"] = "answer";
    j["sdp"] = sdp;   // SDP Answer 字符串
    j["to"] = to;     // 目标对等端 ID
    handleMessage(j.dump());
}

// 发送 ICE Candidate 给指定对等端
// 当本地 ICE Agent 收集到新的候选地址时，通过此方法发送给对端
// 这是 Trickle ICE 的工作方式：候选地址逐个发送，无需等待全部收集完毕
void SignalingClient::sendCandidate(const std::string& candidate,
                                     const std::string& sdpMid,
                                     int sdpMLineIndex,
                                     const std::string& to) {
    json j;
    j["type"] = "candidate";
    j["candidate"] = candidate;       // ICE 候选字符串
    j["sdpMid"] = sdpMid;            // 媒体流 MID
    j["sdpMLineIndex"] = sdpMLineIndex; // m-line 索引
    j["to"] = to;                     // 目标对等端 ID
    handleMessage(j.dump());
}

// 离开当前房间
void SignalingClient::leaveRoom() {
    json j;
    j["type"] = "leave";
    j["room"] = currentRoom_;
    handleMessage(j.dump());
    currentRoom_.clear(); // 清除当前房间标识
}

// 设置消息回调
void SignalingClient::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

// 处理收到的信令消息
// 解析 JSON 消息并转换为 SignalingMessage 结构体，然后触发回调
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
        // 如果消息中没有 peerId 字段，尝试从 "from" 字段获取
        // 服务器转发的消息使用 "from" 字段标识发送方
        if (msg.peerId.empty()) msg.peerId = j.value("from", "");

        // 处理 "joined" 消息：服务器确认加入成功
        // 保存服务器分配的 peerId，后续通信需要使用此 ID
        if (msg.type == "joined") {
            peerId_ = msg.peerId;
            Logger::info("Joined signaling server, peerId={}", peerId_);
        }

        // 触发应用层消息回调
        if (messageCallback_) {
            messageCallback_(msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

} // namespace crystal
