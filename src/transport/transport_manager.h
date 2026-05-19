#pragma once

#include "transport/ice_config.h"
#include <rtc/rtc.hpp>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace crystal {

class PeerConnection {
public:
    using IceCandidateCallback = std::function<void(const std::string& candidate,
                                                     const std::string& sdpMid,
                                                     int sdpMLineIndex)>;
    using TrackCallback = std::function<void(const std::vector<uint8_t>& data)>;
    using StateCallback = std::function<void(rtc::PeerConnection::State state)>;

    explicit PeerConnection(const IceConfig& config);
    ~PeerConnection();

    std::string createOffer();
    std::string createAnswer();
    void setRemoteDescription(const std::string& sdp, const std::string& type);
    void addIceCandidate(const std::string& candidate,
                         const std::string& sdpMid, int sdpMLineIndex);

    void sendMedia(const uint8_t* data, size_t len);
    void sendMedia(const std::vector<uint8_t>& data);

    void onIceCandidate(IceCandidateCallback cb);
    void onTrack(TrackCallback cb);
    void onStateChange(StateCallback cb);

    rtc::PeerConnection::State state() const;
    bool isConnected() const;

private:
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> track_;
    IceConfig config_;
    IceCandidateCallback iceCandidateCb_;
    TrackCallback trackCb_;
    StateCallback stateCb_;
};

class TransportManager {
public:
    explicit TransportManager(const IceConfig& config = IceConfig::defaultConfig());

    std::shared_ptr<PeerConnection> createPeerConnection();
    void destroyPeerConnection(const std::string& id);

private:
    IceConfig config_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<PeerConnection>> connections_;
};

} // namespace crystal
