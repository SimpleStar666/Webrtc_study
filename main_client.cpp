// ============================================================================
// main_client.cpp - CrystalRTC 完整 P2P 客户端
// ============================================================================
//
// 【程序用途】
//   本程序是 CrystalRTC 的完整 P2P 视频通话客户端，连接了所有模块，
//   实现从音视频采集 → 编码 → RTP 打包 → ICE 传输 → RTP 解包 → 解码 → 渲染
//   的完整双向通信链路。
//
// 【运行方式】
//   ./client [房间名] [信令服务器地址] [信令服务器端口]
//   默认值：房间名=default, 服务器=127.0.0.1, 端口=8765
//   示例：./client room1 192.168.1.100 8765
//
// 【涉及的模块】
//   - SignalingClient  : 信令客户端，与信令服务器通信，交换 SDP 和 ICE Candidate
//   - TransportManager : 传输管理器，封装 ICE/DTLS/SRTP，建立 P2P 连接
//   - V4L2Capture      : Linux V4L2 视频采集，从摄像头获取 YUV 帧
//   - H264Encoder      : H.264 视频编码器，将 YUV 帧编码为 NAL Unit
//   - H264Decoder      : H.264 视频解码器，将 NAL Unit 解码为 YUV 帧
//   - SDLRenderer      : SDL 视频渲染器，将 YUV 帧显示到窗口
//   - AlsaCapture      : Linux ALSA 音频采集，从麦克风获取 PCM 数据
//   - OpusEncoder      : Opus 音频编码器，将 PCM 编码为 Opus 帧
//   - OpusDecoder      : Opus 音频解码器，将 Opus 帧解码为 PCM
//   - SDLAudioPlayer   : SDL 音频播放器，将 PCM 数据播放到扬声器
//   - RtpPacketizer    : RTP 打包器，将 NAL/Opus 打包为 RTP 包
//   - RtpDepacketizer  : RTP 解包器，从 RTP 包提取 NAL/Opus
//   - JitterBuffer     : 抖动缓冲区，重排乱序 RTP 包，检测丢包
//
// 【模块协作关系 - 完整数据流】
//
//   ┌──────────────── 发送链路（本地→远端）────────────────┐
//   │                                                      │
//   │  摄像头 → [V4L2Capture] → YUV帧                     │
//   │     → [H264Encoder] → NAL Unit                       │
//   │     → [RtpPacketizer] → RTP包                        │
//   │     → [TransportManager/PeerConnection] → 网络        │
//   │                                                      │
//   │  麦克风 → [AlsaCapture] → PCM                        │
//   │     → [OpusEncoder] → Opus帧                         │
//   │     → [RtpPacketizer] → RTP包                        │
//   │     → [TransportManager/PeerConnection] → 网络        │
//   └──────────────────────────────────────────────────────┘
//
//   ┌──────────────── 接收链路（远端→本地）────────────────┐
//   │                                                      │
//   │  网络 → [TransportManager/PeerConnection] → RTP包    │
//   │     → [JitterBuffer] → 有序RTP包                     │
//   │     → [RtpDepacketizer] → NAL Unit                   │
//   │     → [H264Decoder] → YUV帧                          │
//   │     → [SDLRenderer] → 屏幕                           │
//   │                                                      │
//   │  网络 → [TransportManager/PeerConnection] → RTP包    │
//   │     → [JitterBuffer] → 有序RTP包                     │
//   │     → [RtpDepacketizer] → Opus帧                     │
//   │     → [OpusDecoder] → PCM                            │
//   │     → [SDLAudioPlayer] → 扬声器                      │
//   └──────────────────────────────────────────────────────┘
//
//   ┌──────────────── 信令链路 ────────────────────────┐
//   │                                                  │
//   │  [SignalingClient] ←→ 信令服务器 ←→ 对端客户端   │
//   │     交换: Offer/Answer (SDP) + ICE Candidate     │
//   └──────────────────────────────────────────────────┘
//
// ============================================================================

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

// 全局运行标志，用于优雅退出
static std::atomic<bool> g_running{true};

// 信号处理函数：捕获 Ctrl+C (SIGINT)，设置运行标志为 false 以退出主循环
static void signalHandler(int) {
    g_running = false;
}

// ============================================================================
// 主函数：P2P 客户端入口
// ============================================================================
// 初始化顺序说明：
//   1. 解析命令行参数
//   2. 创建传输层（ICE/DTLS/SRTP）
//   3. 创建 RTP 打包/解包/抖动缓冲区
//   4. 创建音视频编解码器
//   5. 创建音视频采集/渲染设备
//   6. 注册回调（数据流连接）
//   7. 初始化各模块
//   8. 连接信令服务器并加入房间
//   9. 启动音视频采集
//  10. 进入主循环
// ============================================================================
int main(int argc, char* argv[]) {
    // 注册信号处理，支持 Ctrl+C 优雅退出
    std::signal(SIGINT, signalHandler);
    crystal::Logger::init("client");

    // ---- 步骤1：解析命令行参数 ----
    std::string signalingHost = "127.0.0.1";  // 信令服务器地址，默认本机
    uint16_t signalingPort = 8765;             // 信令服务器端口
    std::string room = "default";              // 房间名

    if (argc >= 2) room = argv[1];                                // 第1个参数：房间名
    if (argc >= 3) signalingHost = argv[2];                       // 第2个参数：信令服务器地址
    if (argc >= 4) signalingPort = static_cast<uint16_t>(std::stoi(argv[3]));  // 第3个参数：端口

    crystal::Logger::info("CrystalRTC Client starting...");
    crystal::Logger::info("Room: {}, Server: {}:{}", room, signalingHost, signalingPort);

    // ---- 步骤2：创建传输层 ----
    // TransportManager 封装了 ICE Agent、DTLS 握手和 SRTP 加密，
    // 负责在两个对等端之间建立安全的 P2P 连接
    crystal::IceConfig iceConfig = crystal::IceConfig::defaultConfig();
    crystal::TransportManager transportMgr(iceConfig);

    // 创建 PeerConnection，代表与一个远端对等端的连接
    auto pc = transportMgr.createPeerConnection();

    // ---- 步骤3：创建 RTP 打包/解包/抖动缓冲区 ----
    // 视频打包器：PT=96（动态类型，H.264），时钟=90000Hz，SSRC=0x12345678
    crystal::RtpPacketizer videoPacketizer(96, 90000, 0x12345678);
    // 音频打包器：PT=97（动态类型，Opus），时钟=48000Hz，SSRC=0x87654321
    crystal::RtpPacketizer audioPacketizer(97, 48000, 0x87654321);
    // 解包器：从 RTP 包中提取 H.264 NAL 或 Opus 帧
    crystal::RtpDepacketizer depacketizer;
    // 视频抖动缓冲区：缓存40个包，解决乱序和抖动
    crystal::JitterBuffer videoJitterBuf(40);
    // 音频抖动缓冲区：缓存40个包
    crystal::JitterBuffer audioJitterBuf(40);

    // ---- 步骤4：创建音视频编解码器 ----
    // 视频编码器：将 YUV 帧编码为 H.264 NAL Unit
    crystal::H264EncoderConfig encConfig;
    crystal::H264Encoder encoder(encConfig);
    // 视频解码器：将 H.264 NAL Unit 解码为 YUV 帧
    crystal::H264Decoder decoder;

    // ---- 步骤5：创建音视频采集/渲染设备 ----
    // V4L2 视频采集：从摄像头获取 YUV 帧（Linux 专用）
    crystal::V4L2Config v4l2Config;
    crystal::V4L2Capture capture(v4l2Config);
    // SDL 渲染器：将 YUV 帧显示到窗口
    crystal::SDLRenderer renderer(encConfig.width, encConfig.height, "CrystalRTC - Local");

    // Opus 音频编码器：将 PCM 编码为 Opus 帧
    crystal::OpusEncoderConfig opusEncConfig;
    crystal::OpusEncoder opusEncoder(opusEncConfig);
    // Opus 音频解码器：将 Opus 帧解码为 PCM
    crystal::OpusDecoder opusDecoder;

    // ALSA 音频采集：从麦克风获取 PCM 数据（Linux 专用）
    crystal::AlsaConfig alsaConfig;
    crystal::AlsaCapture alsaCapture(alsaConfig);
    // SDL 音频播放器：将 PCM 数据播放到扬声器
    crystal::SDLAudioPlayer audioPlayer;

    // ---- 步骤6：创建信令客户端 ----
    // 信令客户端负责与信令服务器通信，交换 SDP Offer/Answer 和 ICE Candidate
    crystal::SignalingClient signalingClient;

    // ====================================================================
    // 注册回调：将各模块的数据流连接起来
    // 以下回调的注册顺序不影响功能，但逻辑上按数据流方向排列
    // ====================================================================

    // ---- 回调1：接收远端媒体数据 ----
    // 当 PeerConnection 收到远端发来的 RTP 包时触发此回调
    // 数据流：网络 → RTP包 → 按PT分流 → JitterBuffer → 解包 → 解码 → 渲染/播放
    pc->onTrack([&](const std::vector<uint8_t>& data) {
        crystal::RtpPacket pkt;
        if (!pkt.parse(data.data(), data.size())) return;  // 解析 RTP 包失败则丢弃

        if (pkt.payloadType() == 96) {
            // 视频流（PT=96）：RTP包 → JitterBuffer → 解包 → H.264解码 → 渲染
            videoJitterBuf.insert(pkt);                    // 送入视频抖动缓冲区
            auto packets = videoJitterBuf.consume();       // 获取有序的 RTP 包
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);  // RTP 解包为 NAL Unit
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());    // NAL 送入解码器
                }
            }
        } else if (pkt.payloadType() == 97) {
            // 音频流（PT=97）：RTP包 → JitterBuffer → 解包 → Opus解码 → 播放
            audioJitterBuf.insert(pkt);                    // 送入音频抖动缓冲区
            auto packets = audioJitterBuf.consume();       // 获取有序的 RTP 包
            for (const auto& p : packets) {
                auto frames = depacketizer.depacketizeOpus(p);  // RTP 解包为 Opus 帧
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());  // Opus 解码为 PCM
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());  // 播放 PCM 音频
                    }
                }
            }
        }
    });

    // ---- 回调2：ICE Candidate 发现 ----
    // 当 ICE Agent 发现本地候选地址时，通过信令服务器发送给对端
    // 这是 WebRTC NAT 穿透的关键步骤
    pc->onIceCandidate([&](const std::string& candidate,
                            const std::string& sdpMid, int sdpMLineIndex) {
        signalingClient.sendCandidate(candidate, sdpMid, sdpMLineIndex, "");
    });

    // ---- 回调3：视频解码输出 → 渲染 ----
    // 解码器输出 YUV 帧，送入 SDL 渲染器显示
    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        renderer.render(yuvData, width, height);
    });

    // ---- 回调4：视频编码输出 → RTP 打包 → 发送 ----
    // 编码器每输出一个 NAL Unit，打包为 RTP 包并通过 PeerConnection 发送
    // 数据流：NAL → [RtpPacketizer] → RTP包 → [PeerConnection] → 网络
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, 0);  // 打包为 RTP（大 NAL 自动 FU-A 分片）
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();           // 序列化为字节流
            pc->sendMedia(data);                   // 通过 P2P 连接发送
        }
    });

    // ---- 回调5：视频采集 → 编码 ----
    // 摄像头每采集一帧 YUV 数据，送入编码器
    // 数据流：摄像头 → [V4L2Capture] → YUV帧 → [H264Encoder] → NAL Unit
    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        encoder.encode(yuvData, len);
    });

    // ---- 回调6：音频采集 → 编码 → RTP 打包 → 发送 ----
    // 麦克风采集 PCM 数据，编码为 Opus，打包为 RTP 包并发送
    // 数据流：麦克风 → [AlsaCapture] → PCM → [OpusEncoder] → Opus帧 → [RtpPacketizer] → RTP包 → 网络
    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());  // PCM → Opus
        if (!opusFrame.empty()) {
            auto packets = audioPacketizer.packetizeOpus(opusFrame, 0);  // Opus → RTP 包
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();           // 序列化为字节流
                pc->sendMedia(data);                   // 通过 P2P 连接发送
            }
        }
    });

    // ---- 回调7：信令消息处理 ----
    // 处理从信令服务器收到的消息，包括对端加入、SDP 交换、ICE Candidate 交换
    // 这是 WebRTC 连接建立的核心流程
    signalingClient.onMessage([&](const crystal::SignalingMessage& msg) {
        if (msg.type == "peer_joined") {
            // 对端加入房间：作为发起方，创建 SDP Offer 并发送
            crystal::Logger::info("Peer joined: {}, sending offer", msg.peerId);
            auto sdp = pc->createOffer();                      // 创建 SDP Offer（包含本地媒体能力）
            signalingClient.sendOffer(sdp, msg.peerId);        // 通过信令服务器发送给对端
        } else if (msg.type == "offer") {
            // 收到对端的 SDP Offer：设置远端描述，创建 SDP Answer 并回复
            crystal::Logger::info("Received offer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "offer");        // 设置远端 SDP
            auto answer = pc->createAnswer();                  // 创建 SDP Answer
            signalingClient.sendAnswer(answer, msg.peerId);    // 回复给对端
        } else if (msg.type == "answer") {
            // 收到对端的 SDP Answer：设置远端描述，完成 SDP 交换
            crystal::Logger::info("Received answer from {}", msg.peerId);
            pc->setRemoteDescription(msg.sdp, "answer");       // 设置远端 SDP
        } else if (msg.type == "candidate") {
            // 收到对端的 ICE Candidate：添加到本地 ICE Agent
            crystal::Logger::debug("Received ICE candidate from {}", msg.peerId);
            pc->addIceCandidate(msg.candidate, msg.sdpMid, msg.sdpMLineIndex);
        }
    });

    // ====================================================================
    // 步骤7：初始化各模块
    // 注意：回调必须在 init() 之前注册，因为 init() 可能立即触发回调
    // ====================================================================

    // 初始化视频编解码器
    if (!encoder.init()) {
        crystal::Logger::error("Failed to init H264 encoder");
        return 1;
    }
    if (!decoder.init()) {
        crystal::Logger::error("Failed to init H264 decoder");
        return 1;
    }
    // 初始化 SDL 渲染器
    if (!renderer.init()) {
        crystal::Logger::error("Failed to init SDL renderer");
        return 1;
    }
    // 初始化音频编解码器
    if (!opusEncoder.init()) {
        crystal::Logger::error("Failed to init Opus encoder");
        return 1;
    }
    if (!opusDecoder.init()) {
        crystal::Logger::error("Failed to init Opus decoder");
        return 1;
    }
    // 初始化音频播放器
    if (!audioPlayer.init()) {
        crystal::Logger::error("Failed to init audio player");
        return 1;
    }

    // ====================================================================
    // 步骤8：连接信令服务器并加入房间
    // ====================================================================
    if (!signalingClient.connect(signalingHost, signalingPort)) {
        crystal::Logger::error("Failed to connect to signaling server");
        return 1;
    }

    // 加入指定房间，等待其他对等端
    signalingClient.joinRoom(room);

    // ====================================================================
    // 步骤9：启动音视频采集
    // 采集设备是可选的，如果不可用（如无摄像头/麦克风），程序仍可运行
    // ====================================================================
    if (!capture.open()) {
        crystal::Logger::warn("Video capture not available, continuing without video");
    } else {
        capture.startCapture();  // 开始采集，触发 onFrame 回调
    }

    if (!alsaCapture.open()) {
        crystal::Logger::warn("Audio capture not available, continuing without audio");
    } else {
        alsaCapture.startCapture();  // 开始采集，触发 onAudio 回调
    }

    crystal::Logger::info("CrystalRTC client running. Press Ctrl+C to quit.");

    // ====================================================================
    // 步骤10：主循环
    // 轮询 SDL 事件（窗口关闭等），同时等待退出信号
    // ====================================================================
    while (g_running && !renderer.shouldQuit()) {
        renderer.pollEvents();  // 处理 SDL 窗口事件
        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps 轮询
    }

    // ====================================================================
    // 优雅退出：按逆序停止各模块
    // ====================================================================
    crystal::Logger::info("Shutting down...");
    capture.stopCapture();          // 停止视频采集
    alsaCapture.stopCapture();      // 停止音频采集
    signalingClient.leaveRoom();    // 离开房间，通知信令服务器
    signalingClient.disconnect();   // 断开信令连接

    return 0;
}
