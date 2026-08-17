// ============================================================================
// signaling_server.h - WebSocket 信令服务器
// ============================================================================
//
// 本文件定义了 CrystalRTC 的信令服务器，负责在 WebRTC 对等端之间转发
// SDP Offer/Answer 和 ICE Candidate 等信令消息。
//
// 【信令在 WebRTC 中的角色】
// WebRTC 标准本身没有定义信令协议，信令机制需要由应用自行实现。信令服务器的
// 核心职责是帮助两个对等端交换以下信息：
//
// 1. SDP Offer/Answer
//    - Offer：呼叫方发起的会话描述，包含其支持的媒体格式、编解码器、
//      传输协议、ICE 参数、DTLS 指纹等
//    - Answer：被叫方回复的会话描述，在 Offer 基础上选择双方共同支持的参数
//    - SDP 交换完成后，双方才知道对方支持哪些媒体格式，以及如何建立连接
//
// 2. ICE Candidate
//    - 每个 ICE Candidate 代表一个可用的网络地址（本地地址、STUN 反射地址、
//      TURN 中继地址等）
//    - 双方需要互相交换 ICE Candidate，ICE 协议才能进行连通性检查
//    - ICE Candidate 可以在 SDP 描述中携带（Trickle ICE 关闭时），
//      也可以单独发送（Trickle ICE 开启时，更高效）
//
// 【WebSocket 信令消息格式】
// 本项目使用 JSON 格式的 WebSocket 消息，消息类型包括：
//   - "join"：加入房间，携带 room 字段
//   - "offer"：SDP Offer，携带 sdp 和 to 字段
//   - "answer"：SDP Answer，携带 sdp 和 to 字段
//   - "candidate"：ICE Candidate，携带 candidate/sdpMid/sdpMLineIndex/to 字段
//   - "leave"：离开房间
//   - "joined"：服务器确认加入成功，携带 peerId
//   - "peer_joined"：通知房间内其他对等端有新人加入
//   - "peer_left"：通知房间内其他对等端有人离开
//
// 【libwebsockets 库】
// 本项目使用 libwebsockets 库实现 WebSocket 服务端和客户端。
// libwebsockets 采用事件驱动的回调机制，通过 lws_callback_reasons 枚举
// 通知应用层各种事件（连接建立、数据接收、连接关闭等）。
//
// ============================================================================

#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <memory>

// 前向声明 libwebsockets 的核心类型，避免在头文件中包含 libwebsockets.h
struct lws;
struct lws_context;

namespace crystal {

// ============================================================================
// SignalingMessage - 信令消息结构体
// ============================================================================
//
// 定义了信令服务器中所有消息的统一格式。不同类型的消息使用不同的字段：
// - join: 使用 type + room
// - offer/answer: 使用 type + sdp + to
// - candidate: 使用 type + candidate + sdpMid + sdpMLineIndex + to
// - leave: 使用 type
// - joined/peer_joined/peer_left: 由服务器生成，包含 peerId
//
struct SignalingMessage {
    // 消息类型，取值：join/offer/answer/candidate/leave/joined/peer_joined/peer_left
    std::string type;

    // SDP 会话描述（仅在 offer/answer 类型时有效）
    // 包含媒体格式、编解码器、ICE 参数、DTLS 指纹等完整会话信息
    std::string sdp;

    // ICE 候选字符串（仅在 candidate 类型时有效）
    // 格式如："candidate:0 1 UDP 2122252543 192.168.1.100 50000 typ host"
    std::string candidate;

    // ICE 候选所属的媒体流 MID（Media Stream Identification）
    // 用于将候选关联到正确的 m-line（媒体描述行）
    std::string sdpMid;

    // ICE 候选所属的 m-line 索引
    // 与 sdpMid 功能类似，是候选关联 m-line 的另一种方式
    int sdpMLineIndex = 0;

    // 消息的目标对等端 ID（点对点消息时使用）
    // offer/answer/candidate 消息需要指定接收方
    std::string to;

    // 房间标识（join 消息时使用）
    // 同一房间内的对等端可以互相发现和通信
    std::string room;

    // 消息发送方的对等端 ID（由服务器填充）
    // 用于标识消息来源，对端收到后知道应回复给谁
    std::string peerId;
};

// ============================================================================
// SignalingServer - WebSocket 信令服务器
// ============================================================================
//
// 基于 libwebsockets 实现的 WebSocket 信令服务器，负责：
// 1. 接受客户端的 WebSocket 连接
// 2. 管理对等端的注册与注销
// 3. 解析和转发信令消息（SDP Offer/Answer、ICE Candidate）
// 4. 支持房间内广播和点对点消息
//
// 设计思路：
// - 使用 libwebsockets 的回调机制处理 WebSocket 事件
// - 使用全局指针 g_server 桥接 C 风格回调与 C++ 对象
// - 使用互斥锁保护对等端映射表的并发访问
// - 消息格式为 JSON，使用 nlohmann/json 库解析
//
class SignalingServer {
public:
    // 构造函数
    // 参数：
    //   host - 监听地址（如 "0.0.0.0" 表示监听所有网络接口）
    //   port - 监听端口号
    SignalingServer(const std::string& host, uint16_t port);

    // 析构函数，自动停止服务器
    ~SignalingServer();

    // 启动信令服务器
    // 此方法会阻塞当前线程，在循环中调用 lws_service 处理 WebSocket 事件
    // 直到调用 stop() 方法才会退出
    void start();

    // 停止信令服务器
    // 设置 running_ 标志为 false，使 start() 中的事件循环退出
    void stop();

    // 消息回调函数类型
    // 当服务器收到并处理完一条信令消息后触发
    // 参数：
    //   peerId - 发送方的对等端 ID
    //   msg    - 解析后的信令消息
    using MessageCallback = std::function<void(const std::string& peerId,
                                               const SignalingMessage& msg)>;

    // 设置消息回调
    // 参数：cb - 回调函数
    void onMessage(MessageCallback cb);

    // 处理收到的信令消息
    // 由 libwebsockets 回调函数调用，解析 JSON 消息并根据类型执行相应逻辑：
    // - join: 将对等端加入房间，通知房间内其他人
    // - offer/answer/candidate: 转发给目标对等端
    // - leave: 将对等端移出房间，通知房间内其他人
    // 参数：
    //   peerId - 发送方的对等端 ID
    //   data   - 原始 JSON 字符串
    void handleMessage(const std::string& peerId, const std::string& data);

    // 向指定对等端发送消息
    // 参数：
    //   peerId - 目标对等端 ID
    //   data   - 要发送的 JSON 字符串
    // 注意：使用 LWS_PRE 前缀缓冲区，这是 libwebsockets 的写入要求
    void sendToPeer(const std::string& peerId, const std::string& data);

    // 向房间内所有对等端广播消息（可排除指定对等端）
    // 参数：
    //   room          - 房间标识
    //   data          - 要广播的 JSON 字符串
    //   excludePeerId - 要排除的对等端 ID（通常为消息发送方，避免回声）
    void broadcastToRoom(const std::string& room, const std::string& data,
                         const std::string& excludePeerId);

    // 注册对等端
    // 将 WebSocket 连接与对等端 ID 和房间关联
    // 参数：
    //   peerId - 对等端 ID
    //   wsi    - libwebsockets 的 WebSocket 实例指针
    //   room   - 加入的房间标识
    void registerPeer(const std::string& peerId, struct lws* wsi, const std::string& room);

    // 注销对等端
    // 从对等端映射表中移除指定对等端
    // 参数：peerId - 要注销的对等端 ID
    void unregisterPeer(const std::string& peerId);

private:
    // 监听地址
    std::string host_;

    // 监听端口
    uint16_t port_;

    // libwebsockets 上下文
    // 包含了 WebSocket 服务器的所有运行时状态，由 lws_create_context 创建
    lws_context* context_ = nullptr;

    // 服务器运行标志
    // 为 true 时 start() 中的事件循环持续运行
    bool running_ = false;

    // 对等端映射表的互斥锁，保护 peers_ 的并发访问
    std::mutex peersMutex_;

    // 对等端信息结构体
    struct PeerInfo {
        // libwebsockets 的 WebSocket 实例指针
        // 用于向该对等端发送数据
        struct lws* wsi = nullptr;

        // 该对等端所在的房间标识
        std::string room;
    };

    // 对等端映射表
    // key: 对等端 ID，value: 对等端信息（WebSocket 连接 + 房间）
    std::unordered_map<std::string, PeerInfo> peers_;

    // 消息回调函数
    MessageCallback messageCallback_;
};

} // namespace crystal
