#include "utils/logger.h"
#include "signaling/signaling_client.h"
#include "transport/transport_manager.h"
#include "media/video/v4l2_capture.h"
#include "media/video/h264_encoder.h"
#include "media/video/h264_decoder.h"
#include "media/video/sdl_renderer.h"
#include "media/audio/alsa_capture.h"
#include "media/audio/opus_encoder.h"
#include "media/audio/opus_decoder.h"
#include "media/audio/sdl_audio_player.h"
#include "media/rtp/rtp_packetizer.h"
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/jitter_buffer.h"
#include "room/room_manager.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>

static std::atomic<bool> g_running{true};

static void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signalHandler);
    crystal::Logger::init("client");

    std::string signalingHost = "127.0.0.1";
    uint16_t signalingPort = 8765;
    std::string room = "default";

    if (argc >= 2) room = argv[1];
    if (argc >= 3) signalingHost = argv[2];
    if (argc >= 4) signalingPort = static_cast<uint16_t>(std::stoi(argv[3]));

    crystal::Logger::info("CrystalRTC Client starting...");
    crystal::Logger::info("Room: {}, Server: {}:{}", room, signalingHost, signalingPort);

    crystal::IceConfig iceConfig = crystal::IceConfig::defaultConfig();
    crystal::TransportManager transportMgr(iceConfig);

    auto pc = transportMgr.createPeerConnection();

    crystal::RtpPacketizer videoPacketizer(96, 90000, 0x12345678);
    crystal::RtpPacketizer audioPacketizer(97, 48000, 0x87654321);
    crystal::RtpDepacketizer depacketizer;
    crystal::JitterBuffer videoJitterBuf(40);
    crystal::JitterBuffer audioJitterBuf(40);

    crystal::H264EncoderConfig encConfig;
    crystal::H264Encoder encoder(encConfig);
    crystal::H264Decoder decoder;

    crystal::V4L2Config v4l2Config;
    crystal::V4L2Capture capture(v4l2Config);

    crystal::SDLRenderer renderer(encConfig.width, encConfig.height, "CrystalRTC - Local");

    crystal::OpusEncoderConfig opusEncConfig;
    crystal::OpusEncoder opusEncoder(opusEncConfig);
    crystal::OpusDecoder opusDecoder;

    crystal::AlsaConfig alsaConfig;
    crystal::AlsaCapture alsaCapture(alsaConfig);
    crystal::SDLAudioPlayer audioPlayer;

    crystal::SignalingClient signalingClient;

    pc->onTrack([&](const std::vector<uint8_t>& data) {
        crystal::RtpPacket pkt;
        if (!pkt.parse(data.data(), data.size())) return;

        if (pkt.payloadType() == 96) {
            videoJitterBuf.insert(pkt);
            auto packets = videoJitterBuf.consume();
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());
                }
            }
        } else if (pkt.payloadType() == 97) {
            audioJitterBuf.insert(pkt);
            auto packets = audioJitterBuf.consume();
            for (const auto& p : packets) {
                auto frames = depacketizer.depacketizeOpus(p);
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());
                    }
                }
            }
        }
    });

    pc->onIceCandidate([&](const std::string& candidate,
                            const std::string& sdpMid, int sdpMLineIndex) {
        signalingClient.sendCandidate(candidate, sdpMid, sdpMLineIndex, "");
    });

    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        renderer.render(yuvData, width, height);
    });

    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, 0);
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();
            pc->sendMedia(data);
        }
    });

    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        encoder.encode(yuvData, len);
    });

    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());
        if (!opusFrame.empty()) {
            auto packets = audioPacketizer.packetizeOpus(opusFrame, 0);
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();
                pc->sendMedia(data);
            }
        }
    });

    signalingClient.onMessage([&](const crystal::SignalingMessage& msg) {
        if (msg.type == "peer_joined") {
            crystal::Logger::info("Peer joined: {}, sending offer", msg.peerId);
            auto sdp = pc->createOffer();
            signalingClient.sendOffer(sdp, msg.peerId);
        } else if (msg.type == "offer") {
            crystal::Logger::info("Received offer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "offer");
            auto answer = pc->createAnswer();
            signalingClient.sendAnswer(answer, msg.peerId);
        } else if (msg.type == "answer") {
            crystal::Logger::info("Received answer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "answer");
        } else if (msg.type == "candidate") {
            crystal::Logger::debug("Received ICE candidate from {}", msg.peerId);
            pc->addIceCandidate(msg.candidate, msg.sdpMid, msg.sdpMLineIndex);
        }
    });

    if (!encoder.init()) {
        crystal::Logger::error("Failed to init H264 encoder");
        return 1;
    }
    if (!decoder.init()) {
        crystal::Logger::error("Failed to init H264 decoder");
        return 1;
    }
    if (!renderer.init()) {
        crystal::Logger::error("Failed to init SDL renderer");
        return 1;
    }
    if (!opusEncoder.init()) {
        crystal::Logger::error("Failed to init Opus encoder");
        return 1;
    }
    if (!opusDecoder.init()) {
        crystal::Logger::error("Failed to init Opus decoder");
        return 1;
    }
    if (!audioPlayer.init()) {
        crystal::Logger::error("Failed to init audio player");
        return 1;
    }

    if (!signalingClient.connect(signalingHost, signalingPort)) {
        crystal::Logger::error("Failed to connect to signaling server");
        return 1;
    }

    signalingClient.joinRoom(room);

    if (!capture.open()) {
        crystal::Logger::warn("Video capture not available, continuing without video");
    } else {
        capture.startCapture();
    }

    if (!alsaCapture.open()) {
        crystal::Logger::warn("Audio capture not available, continuing without audio");
    } else {
        alsaCapture.startCapture();
    }

    crystal::Logger::info("CrystalRTC client running. Press Ctrl+C to quit.");

    while (g_running && !renderer.shouldQuit()) {
        renderer.pollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    crystal::Logger::info("Shutting down...");
    capture.stopCapture();
    alsaCapture.stopCapture();
    signalingClient.leaveRoom();
    signalingClient.disconnect();

    return 0;
}
