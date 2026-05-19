#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <algorithm>

namespace crystal {

struct PeerInfo {
    std::string id;
    std::string room;
    bool hasVideo = false;
    bool hasAudio = false;
};

class RoomManager {
public:
    std::string joinRoom(const std::string& room, const std::string& peerId);
    void leaveRoom(const std::string& peerId);
    std::vector<PeerInfo> getPeersInRoom(const std::string& room);
    PeerInfo getPeerInfo(const std::string& peerId);

    using PeerEventCallback = std::function<void(const std::string& room,
                                                  const std::string& peerId,
                                                  const std::string& event)>;
    void onPeerEvent(PeerEventCallback cb);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, PeerInfo> peers_;
    std::unordered_map<std::string, std::vector<std::string>> rooms_;
    PeerEventCallback peerEventCb_;
};

} // namespace crystal
