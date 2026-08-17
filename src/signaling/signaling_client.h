// ============================================================================
// signaling_client.h - WebSocket 信令客户端
// ============================================================================
//
// 本文件定义了 CrystalRTC 的信令客户端，负责与信令服务器建立 WebSocket 连接，
// 发送和接收信令消息。
//
// 【信令客户端的职责】
// 在 WebRTC 连接建立过程中，信令客户端是"传输层"和"网络"之间的桥梁：
// 1. 连接信令服务器，获取分配的 peerId
// 2. 加入/离开房间
// 3. 发送 SDP Offer/Answer 给指定的对等端
// 4. 发送 ICE Candidate 给指定的对等端
// 5. 接收来自其他对等端的信令消息，触发相应的 WebRTC 操作
//
// 【信令流程时序图】
//
//   客户端A          信令服务器          客户端B
//     |                 |                 |
//     |-- join(room) -->|                 |
//     |<-- joined ------|                 |
//     |                 |<-- join(room) --|
//     |<-- peer_joined -|-- peer_joined ->|
//     |                 |                 |
//     |-- offer(B) ---->|-- offer(A) ---->|
//     |                 |                 | (B 收到 Offer，设置远端描述)
//     |                 |<-- answer(A) ---|
//     |<-- answer(B) --|                 | (A 收到 Answer，设置远端描述)
//     |                 |                 |
//     |-- candidate(B)->|-- candidate(A)->|
//     |                 |<-- candidate(B)-|
//     |<-- candidate(A)-|                 |
//     |                 |                 |
//     ========= P2P 连接建立 =============
//
// ============================================================================

#pragma once

#include "signaling/signaling_server.h"
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>

// 前向声明 libwebsockets 类型
struct lws;
struct lws_context;

namespace crystal {

// ============================================================================
// SignalingClient - WebSocket 信令客户端
// ============================================================================
//
// 基于 libwebsockets 实现的 WebSocket 客户端，用于与信令服务器通信。
//
// 设计思路：
// - 在独立线程中运行 libwebsockets 事件循环（serviceThread）
// - 使用原子变量 connected_ 控制事件循环的退出
// - 提供高级 API（joinRoom/sendOffer/sendAnswer/sendCandidate/leaveRoom）
//   封装 JSON 消息的构造和发送
// - 通过 MessageCallback 将收到的消息通知给上层
//
class SignalingClient {
public:
    // 构造函数
    SignalingClient();

    // 析构函数，自动断开连接
    ~SignalingClient();

    // 连接到信令服务器
    // 参数：
    //   url  - 服务器地址（如 "127.0.0.1"）
    //   port - 服务器端口
    // 返回值：true 连接成功，false 连接失败
    // 流程：
    //   1. 创建 libwebsockets 客户端上下文
    //   2. 配置连接参数（地址、端口、协议等）
    //   3. 发起 WebSocket 连接
    //   4. 启动事件循环线程
    bool connect(const std::string& url, uint16_t port);

    // 断开与信令服务器的连接
    // 停止事件循环线程，销毁 libwebsockets 上下文
    void disconnect();

    // 加入房间
    // 参数：room - 房间标识
    // 发送 {"type":"join","room":"xxx"} 消息给服务器
    // 服务器会回复 "joined" 消息，并通知房间内其他对等端
    void joinRoom(const std::string& room);

    // 发送 SDP Offer 给指定对等端
    // 参数：
    //   sdp - SDP Offer 字符串
    //   to  - 目标对等端的 ID
    void sendOffer(const std::string& sdp, const std::string& to);

    // 发送 SDP Answer 给指定对等端
    // 参数：
    //   sdp - SDP Answer 字符串
    //   to  - 目标对等端的 ID
    void sendAnswer(const std::string& sdp, const std::string& to);

    // 发送 ICE Candidate 给指定对等端
    // 参数：
    //   candidate     - ICE 候选字符串
    //   sdpMid        - 媒体流 MID
    //   sdpMLineIndex - m-line 索引
    //   to            - 目标对等端的 ID
    void sendCandidate(const std::string& candidate,
                       const std::string& sdpMid,
                       int sdpMLineIndex,
                       const std::string& to);

    // 离开当前房间
    // 发送 {"type":"leave"} 消息给服务器
    void leaveRoom();

    // 消息回调函数类型
    // 当收到信令服务器发来的消息时触发
    using MessageCallback = std::function<void(const SignalingMessage& msg)>;

    // 设置消息回调
    // 参数：cb - 回调函数
    void onMessage(MessageCallback cb);

    // 获取本端的 peerId
    // 在收到服务器的 "joined" 确认后才会有值
    const std::string& peerId() const { return peerId_; }

private:
    // 事件循环线程函数
    // 在独立线程中循环调用 lws_service 处理 WebSocket 事件
    void serviceThread();

    // 处理收到的消息
    // 解析 JSON 消息并转换为 SignalingMessage 结构体
    // 参数：data - 原始 JSON 字符串
    void handleMessage(const std::string& data);

    // libwebsockets 上下文
    lws_context* context_ = nullptr;

    // WebSocket 连接实例
    lws* wsi_ = nullptr;

    // 连接状态标志（原子变量，线程安全）
    std::atomic<bool> connected_{false};

    // 事件循环线程
    std::thread serviceThread_;

    // 本端的 peerId，由服务器在 "joined" 消息中分配
    std::string peerId_;

    // 当前所在的房间标识
    std::string currentRoom_;

    // 消息回调函数
    MessageCallback messageCallback_;

    // 发送队列的互斥锁
    std::mutex sendMutex_;

    // 待发送消息队列
    // 当 WebSocket 连接尚未就绪时，消息暂存于此队列
    std::vector<std::string> pendingSends_;
};

} // namespace crystal
