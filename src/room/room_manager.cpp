// ============================================================================
// room_manager.cpp - 房间管理器实现
// ============================================================================
//
// 本文件实现了 RoomManager 类，提供房间和对等端的管理功能。
//
// 【实现要点】
// 1. 双映射表设计
//    - peers_ 提供 peerId -> PeerInfo 的快速查找
//    - rooms_ 提供 roomId -> [peerId] 的快速查找
//    - 两个表需要保持一致性，任何修改操作都要同时更新两个表
//
// 2. 线程安全
//    - 所有公共方法都使用 lock_guard 保护
//    - 确保在多线程环境下（如信令回调与定时器同时访问）数据一致
//
// 3. 房间自动清理
//    - 当最后一个对等端离开房间时，自动从 rooms_ 中移除该房间
//    - 避免内存泄漏和无效房间积累
//
// 4. erase-remove 惯用法
//    - 从 rooms_[room] 列表中移除指定 peerId 时使用 std::remove + erase
//    - 这是 C++ 中从 vector 中按值删除元素的标准模式
//
// ============================================================================

#include "room/room_manager.h"
#include "utils/logger.h"

namespace crystal {

// 对等端加入房间
std::string RoomManager::joinRoom(const std::string& room,
                                   const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 创建对等端信息
    PeerInfo info;
    info.id = peerId;
    info.room = room;

    // 存入对等端映射表
    peers_[peerId] = info;

    // 将对等端 ID 添加到房间列表
    rooms_[room].push_back(peerId);

    Logger::info("Peer {} joined room {}", peerId, room);

    // 触发 "joined" 事件回调
    // 上层可以通过此回调得知有新对等端加入，进而发起 WebRTC 连接
    if (peerEventCb_) {
        peerEventCb_(room, peerId, "joined");
    }

    return room;
}

// 对等端离开房间
void RoomManager::leaveRoom(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 查找对等端信息
    auto it = peers_.find(peerId);
    if (it == peers_.end()) return; // 对等端不存在，直接返回

    // 获取该对等端所在的房间
    std::string room = it->second.room;

    // 从对等端映射表中移除
    peers_.erase(it);

    // 从房间列表中移除该对等端
    // 使用 erase-remove 惯用法：
    //   std::remove 将所有不等于 peerId 的元素移到前面，返回新的逻辑末尾
    //   erase 真正删除从逻辑末尾到原末尾之间的元素
    auto& peerList = rooms_[room];
    peerList.erase(std::remove(peerList.begin(), peerList.end(), peerId),
                   peerList.end());

    // 如果房间为空，自动销毁房间
    // 这避免了空房间在内存中无限积累
    if (peerList.empty()) {
        rooms_.erase(room);
    }

    Logger::info("Peer {} left room {}", peerId, room);

    // 触发 "left" 事件回调
    // 上层可以通过此回调得知有对等端离开，进而关闭对应的 PeerConnection
    if (peerEventCb_) {
        peerEventCb_(room, peerId, "left");
    }
}

// 获取房间内所有对等端的信息
std::vector<PeerInfo> RoomManager::getPeersInRoom(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PeerInfo> result;
    auto it = rooms_.find(room);
    if (it != rooms_.end()) {
        // 遍历房间内的所有对等端 ID
        for (const auto& peerId : it->second) {
            // 从 peers_ 映射表中查找对等端的详细信息
            auto pit = peers_.find(peerId);
            if (pit != peers_.end()) {
                result.push_back(pit->second);
            }
        }
    }
    return result;
}

// 获取指定对等端的信息
PeerInfo RoomManager::getPeerInfo(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end()) return it->second;
    // 对等端不存在，返回默认的空 PeerInfo
    return {};
}

// 设置对等端事件回调
void RoomManager::onPeerEvent(PeerEventCallback cb) {
    peerEventCb_ = std::move(cb);
}

} // namespace crystal
