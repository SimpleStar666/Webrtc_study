// ============================================================================
// signaling_server.cpp - WebSocket 信令服务器实现
// ============================================================================
//
// 本文件实现了 SignalingServer 类，基于 libwebsockets 库提供 WebSocket 信令服务。
//
// 【libwebsockets 回调机制详解】
// libwebsockets 采用 C 风格的回调函数模型，核心概念如下：
//
// 1. 协议（Protocol）
//    - 每个协议定义了名称、回调函数、会话数据大小、接收缓冲区大小等
//    - 本项目定义了名为 "signaling" 的协议
//    - 客户端连接时通过 Sec-WebSocket-Protocol 头选择协议
//
// 2. 回调函数（Callback）
//    - 原型：int callback(struct lws* wsi, enum lws_callback_reasons reason,
//                         void* user, void* in, size_t len)
//    - wsi: 触发回调的 WebSocket 实例
//    - reason: 回调原因（连接建立/数据接收/连接关闭等）
//    - user: 每个连接的私有数据区域（本项目未使用）
//    - in: 事件数据（如接收到的消息内容）
//    - len: 事件数据长度
//
// 3. 事件循环
//    - lws_service(context, timeout) 处理一次事件循环迭代
//    - 本项目在 start() 中循环调用 lws_service，超时 50ms
//    - 在每次迭代中，libwebsockets 检查所有连接的状态并触发相应回调
//
// 4. 全局指针桥接
//    - 由于 C 回调无法直接访问 C++ 对象，使用全局指针 g_server 桥接
//    - 在 start() 中将 this 赋值给 g_server，回调中通过 g_server 访问对象
//    - 这种模式在 C 库的 C++ 封装中很常见
//
// 5. LWS_PRE 缓冲区前缀
//    - libwebsockets 要求发送缓冲区的前 LWS_PRE 字节保留给协议头
//    - 实际数据从 buf.data() + LWS_PRE 开始
//    - 这是 libwebsockets 的特殊要求，用于避免内存拷贝
//
// ============================================================================

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

// 生成随机的对等端 ID
// 使用 Mersenne Twister 随机数生成器（mt19937），生成 64 位随机整数作为 ID
// 在实际生产环境中，可能需要更安全的 ID 生成方案（如 UUID）
static std::string generatePeerId() {
    static std::random_device rd;       // 硬件随机数种子源
    static std::mt19937 gen(rd());      // Mersenne Twister 伪随机数生成器
    static std::uniform_int_distribution<uint64_t> dist; // 均匀分布
    return std::to_string(dist(gen));
}

// 全局信令服务器指针
// 用于在 libwebsockets 的 C 风格回调函数中访问 C++ SignalingServer 对象
static SignalingServer* g_server = nullptr;

// 全局互斥锁，保护 g_wsiToPeerId 的并发访问
static std::mutex g_peerMapMutex;

// WebSocket 实例到对等端 ID 的映射
// 在回调函数中使用，将 lws* 连接标识转换为业务层的 peerId
static std::unordered_map<struct lws*, std::string> g_wsiToPeerId;

// ============================================================================
// callbackProtocol - libwebsockets 协议回调函数
// ============================================================================
//
// 这是 libwebsockets 的核心回调函数，处理所有 WebSocket 事件。
// reason 参数标识事件类型，主要处理以下三种事件：
//
// 1. LWS_CALLBACK_ESTABLISHED - 新的 WebSocket 连接建立
//    - 为新连接生成唯一的 peerId
//    - 将 wsi -> peerId 映射存入全局映射表
//
// 2. LWS_CALLBACK_RECEIVE - 收到 WebSocket 消息
//    - 通过 wsi 查找对应的 peerId
//    - 将消息转发给 SignalingServer::handleMessage 处理
//
// 3. LWS_CALLBACK_CLOSED - WebSocket 连接关闭
//    - 从全局映射表中移除该连接
//    - 通知 SignalingServer 注销该对等端
//    - 发送 "leave" 消息通知房间内其他人
//
static int callbackProtocol(struct lws* wsi, enum lws_callback_reasons reason,
                            void* user, void* in, size_t len) {
    switch (reason) {
    // 新的 WebSocket 连接建立
    case LWS_CALLBACK_ESTABLISHED: {
        std::string peerId = generatePeerId();
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            // 将 wsi 指针映射到 peerId，后续收到消息时可通过 wsi 找到 peerId
            g_wsiToPeerId[wsi] = peerId;
        }
        Logger::info("Signaling: peer connected id={}", peerId);
        break;
    }
    // 收到 WebSocket 消息
    case LWS_CALLBACK_RECEIVE: {
        std::string peerId;
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            auto it = g_wsiToPeerId.find(wsi);
            if (it == g_wsiToPeerId.end()) break; // 未找到对应的 peerId，忽略
            peerId = it->second;
        }
        if (!g_server) break; // 服务器未启动，忽略
        // 将收到的二进制数据转换为字符串
        std::string data(static_cast<const char*>(in), len);
        Logger::debug("Signaling: received from {}: {} bytes", peerId, len);
        // 转发给 SignalingServer 处理业务逻辑
        g_server->handleMessage(peerId, data);
        break;
    }
    // WebSocket 连接关闭
    case LWS_CALLBACK_CLOSED: {
        std::string peerId;
        {
            std::lock_guard<std::mutex> lock(g_peerMapMutex);
            auto it = g_wsiToPeerId.find(wsi);
            if (it == g_wsiToPeerId.end()) break;
            peerId = it->second;
            // 从全局映射表中移除该连接
            g_wsiToPeerId.erase(it);
        }
        Logger::info("Signaling: peer disconnected id={}", peerId);
        if (g_server) {
            // 注销对等端，从房间中移除
            g_server->unregisterPeer(peerId);
            // 发送 leave 消息，通知房间内其他人该对等端已离开
            g_server->handleMessage(peerId, "{\"type\":\"leave\"}");
        }
        break;
    }
    default:
        break;
    }
    return 0; // 返回 0 表示正常处理
}

// ============================================================================
// 协议定义
// ============================================================================
//
// libwebsockets 要求以数组形式定义协议列表，数组以 nullptr 结尾。
// 每个协议定义包含：
// - name: 协议名称，客户端连接时通过 Sec-WebSocket-Protocol 头匹配
// - callback: 协议回调函数
// - per_session_data_size: 每个连接的私有数据大小（0 表示不使用）
// - rx_buffer_size: 接收缓冲区大小（4096 字节，足够容纳信令消息）
// - id: 协议 ID（0）
// - user: 用户数据指针（nullptr）
// - tx_packet_size: 发送包大小限制（0 表示不限制）
//
static const struct lws_protocols protocols[] = {
    {"signaling", callbackProtocol, 0, 4096, 0, nullptr, 0},
    {nullptr, nullptr, 0, 0, 0, nullptr, 0} // 终止标记
};

// ============================================================================
// SignalingServer 实现
// ============================================================================

// 构造函数
SignalingServer::SignalingServer(const std::string& host, uint16_t port)
    : host_(host), port_(port) {}

// 析构函数，确保服务器停止运行
SignalingServer::~SignalingServer() {
    stop();
}

// 启动信令服务器
// 此方法会阻塞当前线程，在事件循环中处理 WebSocket 连接
void SignalingServer::start() {
    // 设置全局服务器指针，使回调函数可以访问此对象
    g_server = this;

    // 初始化 libwebsockets 上下文创建信息
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = port_;              // 监听端口
    info.iface = host_.c_str();     // 监听网络接口
    info.protocols = protocols;     // 协议列表
    info.gid = -1;                  // 组 ID（-1 表示不限制）
    info.uid = -1;                  // 用户 ID（-1 表示不限制）

    // 创建 libwebsockets 上下文
    // 上下文包含了服务器的所有运行时状态
    context_ = lws_create_context(&info);
    if (!context_) {
        Logger::error("Failed to create libwebsocket context");
        return;
    }

    Logger::info("Signaling server started on {}:{}", host_, port_);
    running_ = true;

    // 主事件循环
    // lws_service 每次调用处理一批事件，超时 50ms
    // 这意味着即使没有事件，每 50ms 也会返回一次，检查 running_ 标志
    while (running_) {
        lws_service(context_, 50);
    }

    // 销毁 libwebsockets 上下文，释放所有资源
    lws_context_destroy(context_);
    context_ = nullptr;
    g_server = nullptr; // 清除全局指针
}

// 停止信令服务器
void SignalingServer::stop() {
    running_ = false; // 设置标志，使 start() 中的事件循环退出
}

// 设置消息回调
void SignalingServer::onMessage(MessageCallback cb) {
    messageCallback_ = std::move(cb);
}

// 注册对等端
// 将 WebSocket 连接与对等端 ID 和房间关联
void SignalingServer::registerPeer(const std::string& peerId, struct lws* wsi, const std::string& room) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_[peerId].wsi = wsi;      // 保存 WebSocket 连接指针，用于发送数据
    peers_[peerId].room = room;     // 保存房间标识，用于房间内广播
}

// 注销对等端
// 从对等端映射表中移除指定对等端
void SignalingServer::unregisterPeer(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    peers_.erase(peerId);
}

// 处理信令消息
// 这是信令服务器的核心逻辑，解析 JSON 消息并根据类型执行相应操作
void SignalingServer::handleMessage(const std::string& peerId,
                                     const std::string& data) {
    try {
        // 使用 nlohmann/json 解析 JSON 消息
        json j = json::parse(data);
        SignalingMessage msg;
        msg.type = j.value("type", "");             // 消息类型
        msg.sdp = j.value("sdp", "");               // SDP 描述
        msg.candidate = j.value("candidate", "");    // ICE 候选
        msg.sdpMid = j.value("sdpMid", "");          // 媒体流 MID
        msg.sdpMLineIndex = j.value("sdpMLineIndex", 0); // m-line 索引
        msg.to = j.value("to", "");                  // 目标对等端
        msg.room = j.value("room", "");              // 房间标识
        msg.peerId = peerId;                         // 发送方 ID（由服务器填充）

        // 处理 "join" 消息：对等端请求加入房间
        if (msg.type == "join") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            // 记录该对等端所在的房间
            peers_[peerId].room = msg.room;
            Logger::info("Peer {} joined room {}", peerId, msg.room);

            // 向房间内其他对等端广播 "peer_joined" 通知
            // 排除刚加入的对等端自身，避免收到自己加入的通知
            json notify;
            notify["type"] = "peer_joined";
            notify["peerId"] = peerId;
            broadcastToRoom(msg.room, notify.dump(), peerId);

            // 向刚加入的对等端发送 "joined" 确认
            // 包含其分配到的 peerId，客户端需要保存此 ID
            json welcome;
            welcome["type"] = "joined";
            welcome["peerId"] = peerId;
            sendToPeer(peerId, welcome.dump());

        // 处理 "offer"/"answer"/"candidate" 消息：转发给目标对等端
        // 这些是 WebRTC 连接建立过程中的核心信令消息
        } else if (msg.type == "offer" || msg.type == "answer" ||
                   msg.type == "candidate") {
            if (!msg.to.empty()) {
                // 转发消息给目标对等端，并附加发送方 ID
                // 对端收到后知道应该回复给谁
                json forward = j;
                forward["from"] = peerId;
                sendToPeer(msg.to, forward.dump());
            }

        // 处理 "leave" 消息：对等端离开房间
        } else if (msg.type == "leave") {
            std::lock_guard<std::mutex> lock(peersMutex_);
            // 获取该对等端所在的房间
            std::string room = peers_[peerId].room;
            // 从映射表中移除
            peers_.erase(peerId);

            // 向房间内所有对等端广播 "peer_left" 通知
            // 注意：不排除任何人，让所有人都知道有人离开
            json notify;
            notify["type"] = "peer_left";
            notify["peerId"] = peerId;
            broadcastToRoom(room, notify.dump(), "");
        }

        // 触发应用层消息回调
        if (messageCallback_) {
            messageCallback_(peerId, msg);
        }
    } catch (const json::exception& e) {
        Logger::error("Failed to parse signaling message: {}", e.what());
    }
}

// 向指定对等端发送 WebSocket 消息
void SignalingServer::sendToPeer(const std::string& peerId,
                                  const std::string& data) {
    std::lock_guard<std::mutex> lock(peersMutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end() && it->second.wsi) {
        // 分配发送缓冲区
        // LWS_PRE 是 libwebsockets 要求的缓冲区前缀大小
        // libwebsockets 使用这块空间写入 WebSocket 帧头，避免额外的内存拷贝
        std::vector<uint8_t> buf(LWS_PRE + data.size());
        // 将实际数据写入 LWS_PRE 之后的位置
        std::memcpy(buf.data() + LWS_PRE, data.data(), data.size());
        // 调用 lws_write 发送数据
        // LWS_WRITE_TEXT 表示发送文本帧（JSON 消息）
        lws_write(it->second.wsi, buf.data() + LWS_PRE, data.size(),
                  LWS_WRITE_TEXT);
    }
}

// 向房间内所有对等端广播消息
// 遍历所有对等端，向同房间且非排除的对等端发送消息
void SignalingServer::broadcastToRoom(const std::string& room,
                                       const std::string& data,
                                       const std::string& excludePeerId) {
    for (const auto& [id, info] : peers_) {
        // 排除指定对等端（通常是消息发送方），且只发送给同房间的对等端
        if (id != excludePeerId && info.room == room) {
            sendToPeer(id, data);
        }
    }
}

} // namespace crystal
