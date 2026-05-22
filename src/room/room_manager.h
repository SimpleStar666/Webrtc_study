// ============================================================================
// room_manager.h - 房间管理器
// ============================================================================
//
// 本文件定义了 CrystalRTC 的房间管理模块，负责管理多人音视频会议中的
// 房间和对等端（Peer）信息。
//
// 【房间/对等端管理模型】
// 在 WebRTC 多人会议中，需要一种机制来管理参与者和他们之间的关系。
// CrystalRTC 采用"房间"（Room）模型：
//
// 1. 房间（Room）
//    - 是一个逻辑上的会议空间，用字符串标识（如 "meeting-001"）
//    - 同一房间内的对等端可以互相发现并建立 WebRTC 连接
//    - 房间在对等端加入时自动创建，在所有对等端离开后自动销毁
//
// 2. 对等端（Peer）
//    - 代表一个参与会议的客户端
//    - 每个对等端有唯一的 ID，属于一个房间
//    - 对等端可以拥有视频/音频能力标记
//
// 【WebRTC 多人会议拓扑】
// 本项目采用全网状（Full Mesh）拓扑：
// - 每个参与者与其他每个参与者分别建立一个 PeerConnection
// - 优点：延迟最低，不需要媒体服务器
// - 缺点：带宽和 CPU 消耗随参与者数量呈 O(n²) 增长
// - 适合 2-6 人的小型会议
//
// 其他常见的多人会议拓扑：
// - SFU（Selective Forwarding Unit）：服务器选择性地转发媒体流
//   适合中等规模会议（10-50人）
// - MCU（Multipoint Control Unit）：服务器混流后转发
//   适合大规模会议，但延迟较高
//
// ============================================================================

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <algorithm>

namespace crystal {

// ============================================================================
// PeerInfo - 对等端信息结构体
// ============================================================================
//
// 描述一个参与会议的对等端的基本信息，包括：
// - 唯一标识
// - 所属房间
// - 媒体能力（是否支持视频/音频）
//
struct PeerInfo {
    // 对等端唯一标识
    // 与信令服务器分配的 peerId 一致
    std::string id;

    // 所属房间标识
    std::string room;

    // 是否支持视频
    // 用于协商时判断对端是否可以发送/接收视频
    bool hasVideo = false;

    // 是否支持音频
    // 用于协商时判断对端是否可以发送/接收音频
    bool hasAudio = false;
};

// ============================================================================
// RoomManager - 房间管理器
// ============================================================================
//
// 管理房间和对等端的生命周期，提供以下核心功能：
// 1. 对等端加入/离开房间
// 2. 查询房间内的对等端列表
// 3. 查询指定对等端的信息
// 4. 对等端事件通知（加入/离开）
//
// 设计思路：
// - 使用两个映射表维护数据：
//   peers_: peerId -> PeerInfo（对等端信息）
//   rooms_: roomId -> [peerId]（房间内的对等端列表）
// - 使用互斥锁保护并发访问
// - 通过回调函数通知上层对等端事件
// - 房间自动管理：第一个对等端加入时创建，最后一个离开时销毁
//
class RoomManager {
public:
    // 对等端加入房间
    // 参数：
    //   room   - 房间标识
    //   peerId - 对等端 ID
    // 返回值：加入的房间标识
    // 流程：
    //   1. 创建 PeerInfo 并存入 peers_ 映射表
    //   2. 将 peerId 添加到 rooms_[room] 列表
    //   3. 触发 "joined" 事件回调
    std::string joinRoom(const std::string& room, const std::string& peerId);

    // 对等端离开房间
    // 参数：peerId - 对等端 ID
    // 流程：
    //   1. 从 peers_ 映射表中查找该对等端所在的房间
    //   2. 从 peers_ 和 rooms_ 中移除该对等端
    //   3. 如果房间为空，自动销毁房间
    //   4. 触发 "left" 事件回调
    void leaveRoom(const std::string& peerId);

    // 获取房间内所有对等端的信息
    // 参数：room - 房间标识
    // 返回值：房间内所有对等端的 PeerInfo 列表
    std::vector<PeerInfo> getPeersInRoom(const std::string& room);

    // 获取指定对等端的信息
    // 参数：peerId - 对等端 ID
    // 返回值：对等端的 PeerInfo，如果不存在返回空 PeerInfo
    PeerInfo getPeerInfo(const std::string& peerId);

    // 对等端事件回调函数类型
    // 参数：
    //   room   - 房间标识
    //   peerId - 对等端 ID
    //   event  - 事件类型（"joined" 或 "left"）
    using PeerEventCallback = std::function<void(const std::string& room,
                                                  const std::string& peerId,
                                                  const std::string& event)>;

    // 设置对等端事件回调
    // 参数：cb - 回调函数
    void onPeerEvent(PeerEventCallback cb);

private:
    // 互斥锁，保护 peers_ 和 rooms_ 的并发访问
    std::mutex mutex_;

    // 对等端映射表
    // key: 对等端 ID，value: 对等端信息
    std::unordered_map<std::string, PeerInfo> peers_;

    // 房间映射表
    // key: 房间标识，value: 房间内的对等端 ID 列表
    std::unordered_map<std::string, std::vector<std::string>> rooms_;

    // 对等端事件回调函数
    PeerEventCallback peerEventCb_;
};

} // namespace crystal
