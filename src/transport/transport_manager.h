// ============================================================================
// transport_manager.h - 传输管理器（PeerConnection 与 TransportManager）
// ============================================================================
//
// 本文件定义了 CrystalRTC 的传输层核心组件，负责 WebRTC 点对点连接的创建、
// 管理和媒体数据传输。
//
// 【WebRTC 连接建立流程概述】
// WebRTC 建立连接需要经过以下关键步骤：
//
// 1. 信令交换（通过 SignalingServer/SignalingClient 完成）
//    - 交换 SDP Offer/Answer：双方协商媒体编解码器、传输协议等参数
//    - 交换 ICE Candidate：双方互相告知可用的网络地址候选
//
// 2. ICE 连接检测
//    - 双方对收集到的候选地址进行连通性检查（Connectivity Check）
//    - 使用 STUN Binding Request/Response 验证候选地址对是否可达
//    - 最终选择一对最优的候选地址作为通信路径
//
// 3. DTLS-SRTP 密钥协商
//    - 在 ICE 连接建立后，通过 DTLS 握手协商 SRTP 加密密钥
//    - DTLS（Datagram TLS）是 TLS 在 UDP 上的实现
//    - SRTP（Secure RTP）使用 DTLS 协商出的密钥对媒体流加密
//    - 这确保了音视频数据的端到端加密，即使中继服务器也无法解密
//
// 4. 媒体传输
//    - 加密的音视频数据通过 SRTP 协议传输
//    - RTP（Real-time Transport Protocol）负责媒体数据传输
//    - RTCP（RTP Control Protocol）负责传输质量反馈
//
// 【libdatachannel 库】
// 本项目使用 libdatachannel 库（rtc::PeerConnection, rtc::Track 等）来实现
// WebRTC 协议栈，它封装了 ICE、DTLS、SRTP 等底层细节。
// 注意：rtc::binary 类型是 std::vector<std::byte>，不是 std::vector<uint8_t>，
// 在与 uint8_t 数据交互时需要进行类型转换。
//
// ============================================================================

#pragma once

#include "transport/ice_config.h"
#include <rtc/rtc.hpp>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace crystal {

// ============================================================================
// PeerConnection - 单个 WebRTC 对等连接
// ============================================================================
//
// 封装了 libdatachannel 的 rtc::PeerConnection，代表与一个远端对等端的
// WebRTC 连接。每个 PeerConnection 包含：
// - ICE 协商（候选地址收集与交换）
// - SDP Offer/Answer 处理
// - 媒体轨道（Track）的添加与数据收发
// - 连接状态监控
//
// 典型使用流程：
//   呼叫方（Caller）：
//     1. createOffer() -> 生成 SDP Offer，添加媒体轨道
//     2. 将 Offer 通过信令服务器发送给对方
//     3. 收到对方的 Answer 后调用 setRemoteDescription()
//     4. 收集 ICE Candidate 并通过信令发送给对方
//     5. 收到对方的 ICE Candidate 后调用 addIceCandidate()
//
//   被叫方（Callee）：
//     1. 收到 Offer 后调用 setRemoteDescription()
//     2. createAnswer() -> 生成 SDP Answer
//     3. 将 Answer 通过信令服务器发送给对方
//     4. 收集并发送 ICE Candidate
//     5. 收到对方的 ICE Candidate 后调用 addIceCandidate()
//
class PeerConnection {
public:
    // ICE 候选回调函数类型
    // 当本地收集到新的 ICE Candidate 时触发
    // 参数：
    //   candidate - ICE 候选字符串，如 "a=candidate:0 1 UDP 2122252543 192.168.1.100 50000 typ host"
    //   sdpMid    - 候选所属的媒体流标识（m-line 的 ID），如 "0"
    //   sdpMLineIndex - 候选所属的 m-line 索引（从 0 开始）
    using IceCandidateCallback = std::function<void(const std::string& candidate,
                                                     const std::string& sdpMid,
                                                     int sdpMLineIndex)>;

    // 媒体轨道数据接收回调函数类型
    // 当从远端接收到媒体数据（音视频 RTP 包）时触发
    // 参数：data - 接收到的媒体数据（已从 rtc::binary 转换为 std::vector<uint8_t>）
    using TrackCallback = std::function<void(const std::vector<uint8_t>& data)>;

    // RTCP 数据接收回调函数类型
    // PeerConnection 内部已按 RFC 5761 完成 RTP/RTCP 去复用，
    // 本回调只收到 RTCP 报文（可能是含多个子包的复合包）
    using RtcpCallback = std::function<void(const std::vector<uint8_t>& data)>;

    // 连接状态变化回调函数类型
    // 当 PeerConnection 的状态发生变化时触发
    // 参数：state - 新的连接状态（New/Connecting/Connected/Disconnected/Failed/Closed）
    using StateCallback = std::function<void(rtc::PeerConnection::State state)>;

    // 构造函数
    // 参数：config - ICE 配置，包含 STUN/TURN 服务器信息
    explicit PeerConnection(const IceConfig& config);

    // 析构函数，关闭 PeerConnection 释放资源
    ~PeerConnection();

    // 创建 SDP Offer（呼叫方调用）
    // 流程：
    //   1. 添加一个视频媒体轨道（Track），方向为 SendRecv（可收可发）
    //   2. 设置轨道的消息回调以接收远端媒体数据
    //   3. 触发 ICE 候选收集
    //   4. 返回本地 SDP 描述
    // 返回值：SDP Offer 字符串，如果生成失败返回空字符串
    std::string createOffer();

    // 创建 SDP Answer（被叫方调用）
    // 在 setRemoteDescription() 之后调用，返回本地 SDP 描述
    // 返回值：SDP Answer 字符串，如果生成失败返回空字符串
    std::string createAnswer();

    // 设置远端 SDP 描述
    // 参数：
    //   sdp  - 远端的 SDP 字符串（Offer 或 Answer）
    //   type - SDP 类型，"offer" 或 "answer"
    // 流程：
    //   1. 根据 type 构造 rtc::Description 对象
    //   2. 调用 libdatachannel 的 setRemoteDescription 设置远端描述
    //   3. 如果本地还没有 Track，注册 onTrack 回调等待远端轨道
    //   4. 设置远端描述后会触发 ICE 候选收集和 DTLS 握手
    void setRemoteDescription(const std::string& sdp, const std::string& type);

    // 添加远端 ICE 候选
    // 参数：
    //   candidate     - ICE 候选字符串
    //   sdpMid        - 媒体流标识
    //   sdpMLineIndex - m-line 索引
    // 将远端发送过来的 ICE Candidate 添加到本地候选列表中，
    // ICE 协议会将其纳入连通性检查
    void addIceCandidate(const std::string& candidate,
                         const std::string& sdpMid, int sdpMLineIndex);

    // 发送媒体数据（原始指针版本）
    // 参数：
    //   data - 指向媒体数据的指针（如编码后的视频帧）
    //   len  - 数据长度（字节）
    // 通过已建立的 Track 发送数据，数据会被 SRTP 加密后传输
    void sendMedia(const uint8_t* data, size_t len);

    // 发送媒体数据（vector 版本）
    // 参数：data - 包含媒体数据的 vector
    void sendMedia(const std::vector<uint8_t>& data);

    // 发送 RTCP 报文（与 RTP 同 Track 同端口复用，RFC 5761）
    // track 未就绪时静默丢弃（设计文档错误处理表约定）
    void sendRtcp(const std::vector<uint8_t>& data);

    // 设置 RTCP 接收回调
    // 参数：cb - 回调函数，收到远端 RTCP 复合包时调用
    void onRtcp(RtcpCallback cb);

    // 设置 ICE 候选回调
    // 参数：cb - 回调函数，当本地收集到新的 ICE Candidate 时调用
    void onIceCandidate(IceCandidateCallback cb);

    // 设置媒体轨道数据接收回调
    // 参数：cb - 回调函数，当收到远端媒体数据时调用
    void onTrack(TrackCallback cb);

    // 设置连接状态变化回调
    // 参数：cb - 回调函数，当连接状态改变时调用
    void onStateChange(StateCallback cb);

    // 获取当前连接状态
    // 返回值：PeerConnection 的当前状态
    rtc::PeerConnection::State state() const;

    // 检查连接是否已建立
    // 返回值：true 表示连接已建立（State::Connected），false 表示未连接
    bool isConnected() const;

private:
    // libdatachannel 的 PeerConnection 实例
    // 这是 WebRTC 协议栈的核心对象，管理 ICE、DTLS、SRTP 等所有底层协议
    std::shared_ptr<rtc::PeerConnection> pc_;

    // 媒体轨道（Track）
    // 在 WebRTC 中，Track 代表一条媒体流（如视频流或音频流）
    // 通过 Track 可以发送和接收媒体数据
    std::shared_ptr<rtc::Track> track_;

    // ICE 配置（STUN/TURN 服务器信息）
    IceConfig config_;

    // ICE 候选回调
    IceCandidateCallback iceCandidateCb_;

    // 媒体数据接收回调
    TrackCallback trackCb_;

    // RTCP 数据接收回调
    RtcpCallback rtcpCb_;

    // 统一安装 Track 的消息回调（RTP/RTCP 去复用后分发）
    // createOffer 与 onTrack 两处创建 Track 都走此函数，避免逻辑重复
    void installTrackHandler();

    // 连接状态变化回调
    StateCallback stateCb_;
};

// ============================================================================
// TransportManager - 传输管理器
// ============================================================================
//
// 管理多个 PeerConnection 的生命周期。在一个多人音视频会议中，每个参与者
// 需要与其他每个参与者分别建立一个 PeerConnection（全网状拓扑/Mesh）。
// TransportManager 负责创建、存储和销毁这些连接。
//
// 设计思路：
// - 使用互斥锁保护 connections_ 映射表，确保线程安全
// - 每个 PeerConnection 分配一个唯一 ID（如 "pc_0", "pc_1"）
// - 通过 shared_ptr 管理 PeerConnection 的生命周期，允许外部持有引用
//
class TransportManager {
public:
    // 构造函数
    // 参数：config - ICE 配置，默认使用 defaultConfig（Google STUN 服务器）
    explicit TransportManager(const IceConfig& config = IceConfig::defaultConfig());

    // 创建一个新的 PeerConnection
    // 返回值：新创建的 PeerConnection 的 shared_ptr
    // 注意：此方法是线程安全的
    std::shared_ptr<PeerConnection> createPeerConnection();

    // 销毁指定 ID 的 PeerConnection
    // 参数：id - 要销毁的 PeerConnection 的 ID
    // 注意：此方法是线程安全的
    void destroyPeerConnection(const std::string& id);

private:
    // ICE 配置，所有新建的 PeerConnection 共享此配置
    IceConfig config_;

    // 互斥锁，保护 connections_ 的并发访问
    std::mutex mutex_;

    // PeerConnection 映射表
    // key: 连接 ID（如 "pc_0"），value: PeerConnection 的 shared_ptr
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> connections_;
};

} // namespace crystal
