#include "room/room_manager.h"
#include "utils/logger.h"

namespace crystal {

std::string RoomManager::joinRoom(const std::string& room,
                                   const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    PeerInfo info;
    info.id = peerId;
    info.room = room;
    peers_[peerId] = info;
    rooms_[room].push_back(peerId);

    Logger::info("Peer {} joined room {}", peerId, room);

    if (peerEventCb_) {
        peerEventCb_(room, peerId, "joined");
    }

    return room;
}

void RoomManager::leaveRoom(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = peers_.find(peerId);
    if (it == peers_.end()) return;

    std::string room = it->second.room;
    peers_.erase(it);

    auto& peerList = rooms_[room];
    peerList.erase(std::remove(peerList.begin(), peerList.end(), peerId),
                   peerList.end());

    if (peerList.empty()) {
        rooms_.erase(room);
    }

    Logger::info("Peer {} left room {}", peerId, room);

    if (peerEventCb_) {
        peerEventCb_(room, peerId, "left");
    }
}

std::vector<PeerInfo> RoomManager::getPeersInRoom(const std::string& room) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<PeerInfo> result;
    auto it = rooms_.find(room);
    if (it != rooms_.end()) {
        for (const auto& peerId : it->second) {
            auto pit = peers_.find(peerId);
            if (pit != peers_.end()) {
                result.push_back(pit->second);
            }
        }
    }
    return result;
}

PeerInfo RoomManager::getPeerInfo(const std::string& peerId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(peerId);
    if (it != peers_.end()) return it->second;
    return {};
}

void RoomManager::onPeerEvent(PeerEventCallback cb) {
    peerEventCb_ = std::move(cb);
}

} // namespace crystal
