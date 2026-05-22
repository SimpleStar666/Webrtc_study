#include "transport/transport_manager.h"
#include "utils/logger.h"
#include <rtc/rtc.hpp>
#include <random>
#include <sstream>

namespace crystal {

static std::vector<uint8_t> binaryToVec(const rtc::binary& bin) {
    std::vector<uint8_t> result(bin.size());
    for (size_t i = 0; i < bin.size(); i++) {
        result[i] = static_cast<uint8_t>(bin[i]);
    }
    return result;
}

static rtc::binary vecToBinary(const uint8_t* data, size_t len) {
    rtc::binary result(len);
    for (size_t i = 0; i < len; i++) {
        result[i] = static_cast<std::byte>(data[i]);
    }
    return result;
}

PeerConnection::PeerConnection(const IceConfig& config) : config_(config) {
    rtc::InitLogger(rtc::LogLevel::Warning);

    rtc::Configuration rtcConfig;
    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }
    for (const auto& turn : config_.turnServers) {
        rtcConfig.iceServers.emplace_back(turn, 3478,
                                           config_.turnUsername,
                                           config_.turnPassword);
    }

    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    pc_->onLocalDescription([this](const rtc::Description& desc) {
        Logger::debug("Local description created: type={}",
                       desc.typeString());
    });

    pc_->onLocalCandidate([this](const rtc::Candidate& cand) {
        Logger::debug("Local ICE candidate: {}",
                       std::string(cand.candidate()));
        if (iceCandidateCb_) {
            iceCandidateCb_(std::string(cand.candidate()),
                           std::string(cand.mid()), 0);
        }
    });

    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        Logger::info("PeerConnection state: {}", static_cast<int>(state));
        if (stateCb_) stateCb_(state);
    });

    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        Logger::debug("ICE gathering state: {}", static_cast<int>(state));
    });
}

PeerConnection::~PeerConnection() {
    if (pc_) {
        pc_->close();
    }
}

std::string PeerConnection::createOffer() {
    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    auto track = pc_->addTrack(video);
    if (track) {
        track_ = track;
        track_->onMessage([this](const rtc::binary& data) {
            if (trackCb_) {
                trackCb_(binaryToVec(data));
            }
        }, nullptr);
    }

    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }

    return "";
}

std::string PeerConnection::createAnswer() {
    auto desc = pc_->localDescription();
    if (desc) {
        return std::string(*desc);
    }
    return "";
}

void PeerConnection::setRemoteDescription(const std::string& sdp,
                                           const std::string& type) {
    rtc::Description desc(sdp, type == "answer" ? rtc::Description::Type::Answer
                                                : rtc::Description::Type::Offer);
    pc_->setRemoteDescription(desc);

    if (!track_) {
        pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
            track_ = track;
            track_->onMessage([this](const rtc::binary& data) {
                if (trackCb_) {
                    trackCb_(binaryToVec(data));
                }
            }, nullptr);
        });
    }
}

void PeerConnection::addIceCandidate(const std::string& candidate,
                                      const std::string& sdpMid,
                                      int sdpMLineIndex) {
    rtc::Candidate cand(candidate, sdpMid);
    pc_->addRemoteCandidate(cand);
}

void PeerConnection::sendMedia(const uint8_t* data, size_t len) {
    if (track_ && track_->isOpen()) {
        auto bin = vecToBinary(data, len);
        track_->send(bin);
    }
}

void PeerConnection::sendMedia(const std::vector<uint8_t>& data) {
    sendMedia(data.data(), data.size());
}

void PeerConnection::onIceCandidate(IceCandidateCallback cb) {
    iceCandidateCb_ = std::move(cb);
}

void PeerConnection::onTrack(TrackCallback cb) {
    trackCb_ = std::move(cb);
}

void PeerConnection::onStateChange(StateCallback cb) {
    stateCb_ = std::move(cb);
}

rtc::PeerConnection::State PeerConnection::state() const {
    return pc_->state();
}

bool PeerConnection::isConnected() const {
    return pc_->state() == rtc::PeerConnection::State::Connected;
}

TransportManager::TransportManager(const IceConfig& config)
    : config_(config) {}

std::shared_ptr<PeerConnection> TransportManager::createPeerConnection() {
    std::lock_guard<std::mutex> lock(mutex_);
    static int counter = 0;
    std::string id = "pc_" + std::to_string(counter++);
    auto pc = std::make_shared<PeerConnection>(config_);
    connections_[id] = pc;
    Logger::info("Created PeerConnection: {}", id);
    return pc;
}

void TransportManager::destroyPeerConnection(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(id);
    Logger::info("Destroyed PeerConnection: {}", id);
}

} // namespace crystal
