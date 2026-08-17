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
#include "media/rtcp/rtcp_packet.h"
#include "media/rtcp/retransmission_buffer.h"
#include "media/rtcp/nack_requester.h"
#include "media/rtcp/rtcp_reporter.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <random>
#include <chrono>

// 全局运行标志，用于优雅退出
static std::atomic<bool> g_running{true};

// 信号处理函数：捕获 Ctrl+C (SIGINT)，设置运行标志为 false 以退出主循环
static void signalHandler(int) {
    g_running = false;
}

// 单调时钟毫秒数（RTCP 组件的统一时间源，避免系统时间跳变影响）
static uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
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
    // SSRC 随机生成：Phase 1 两端都写死固定值，RTCP 报告块将无法
    // 区分统计属于哪条流；随机化后每条流全局唯一（RFC 3550 8.1 建议）。
    // 初始序列号同样随机化（RFC 3550 5.1：增大攻击者猜测难度）
    std::random_device rd;
    uint32_t videoSsrc = rd();
    uint32_t audioSsrc = rd();
    // 视频打包器：PT=96（H.264），时钟=90000Hz
    crystal::RtpPacketizer videoPacketizer(96, 90000, videoSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF));
    // 音频打包器：PT=97（Opus），时钟=48000Hz
    crystal::RtpPacketizer audioPacketizer(97, 48000, audioSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF));
    // 解包器：从 RTP 包中提取 H.264 NAL 或 Opus 帧
    crystal::RtpDepacketizer depacketizer;
    // 视频抖动缓冲区：解决乱序和抖动，并检测丢失 seq（NACK 数据源）
    crystal::JitterBuffer videoJitterBuf(40);
    // 音频抖动缓冲区
    crystal::JitterBuffer audioJitterBuf(40);

    // ---- 步骤3b：RTCP 反馈组件 ----
    // 发送侧：已发视频包缓存，响应对端 NACK 补发（音频不做 NACK——
    // 重传到达已错过播放时刻，Opus 自带 PLC 丢包隐藏更划算）
    crystal::RetransmissionBuffer retxBuffer;
    // 接收侧：丢失包请求状态机（立即请求 + 33ms 重试 + 3 次放弃）
    crystal::NackRequester nackRequester;
    // 统计：发送侧构造 SR / 接收侧构造 RR 报告块（每流一对）
    crystal::SendSideReporter videoSendReport(videoSsrc, "crystal-video");
    crystal::SendSideReporter audioSendReport(audioSsrc, "crystal-audio");
    crystal::RecvSideReporter videoRecvReport(90000);  // 收对端视频(PT=96)
    crystal::RecvSideReporter audioRecvReport(48000);  // 收对端音频(PT=97)
    // 本端是否已发出过包（决定周期发 SR 还是纯 RR，RFC 3550 6.4）
    bool videoSent = false, audioSent = false;
    // 当前帧的 RTP 时间戳（修复 Phase 1 全 0 时间戳问题，见回调5）
    uint32_t videoRtpTs = 0, audioRtpTs = 0;

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

    // ---- 辅助：发 PLI（500ms 节流）----
    // 关键帧体积是 P 帧数倍，无节流会造成"错误→PLI→大帧→更易丢→错误"
    // 的正反馈风暴，挤占正常媒体带宽
    uint64_t lastPliSentMs = 0;
    auto sendPli = [&]() {
        if (!videoRecvReport.active()) return;   // 还不知道对端视频 SSRC
        uint64_t now = nowMs();
        if (now - lastPliSentMs < 500) return;   // 节流
        lastPliSentMs = now;
        crystal::PliPacket pli;
        pli.senderSsrc = videoSsrc;              // 发起方（本端）SSRC
        pli.mediaSsrc = videoRecvReport.remoteSsrc();  // 被请求的媒体流
        std::vector<uint8_t> buf;
        crystal::appendPli(buf, pli);
        pc->sendRtcp(buf);
        crystal::Logger::info("RTCP: PLI sent (keyframe requested)");
    };

    // ---- 辅助：发 NACK（seq 列表 → PID+BLP 位图压缩）----
    auto sendNack = [&](const std::vector<uint16_t>& seqs) {
        if (seqs.empty() || !videoRecvReport.active()) return;
        crystal::NackPacket nack;
        nack.senderSsrc = videoSsrc;
        nack.mediaSsrc = videoRecvReport.remoteSsrc();
        nack.entries = crystal::NackPacket::buildEntries(seqs);
        std::vector<uint8_t> buf;
        crystal::appendNack(buf, nack);
        pc->sendRtcp(buf);
    };

    // ---- 回调1：接收远端媒体数据 ----
    // 当 PeerConnection 收到远端发来的 RTP 包时触发此回调
    // 数据流：网络 → RTP包 → 统计+NACK检测 → JitterBuffer → 解包 → 解码 → 渲染/播放
    pc->onTrack([&](const std::vector<uint8_t>& data) {
        crystal::RtpPacket pkt;
        if (!pkt.parse(data.data(), data.size())) return;  // 解析 RTP 包失败则丢弃

        if (pkt.payloadType() == 96) {
            // === 接收统计（RR 数据源）：丢包/回绕/抖动 ===
            videoRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            // === NACK 状态机联动 ===
            // 任意到达的包（含重传包）都解除对应 seq 的待请求状态
            nackRequester.onReceived(pkt.sequenceNumber());
            // insert 返回本次新检测到的丢失 seq（≤64 的小间隙才有值）
            auto missing = videoJitterBuf.insert(pkt);
            // 状态机过滤出"现在就该请求"的 seq（其余进入重试队列）
            auto toRequest = nackRequester.onMissing(missing, nowMs());
            sendNack(toRequest);

            auto packets = videoJitterBuf.consume();       // 获取有序的 RTP 包
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);  // RTP 解包为 NAL Unit
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());    // NAL 送入解码器
                }
            }
        } else if (pkt.payloadType() == 97) {
            // 音频只做统计（不做 NACK：重传到达已错过播放时刻，
            // Opus 自带 PLC 丢包隐藏更划算）
            audioRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
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

    // ---- 回调1b：接收远端 RTCP 复合包（传输层去复用后到达这里）----
    // NACK → 补发 | PLI → 强制关键帧 | RR → RTT | SR → LSR 基准
    pc->onRtcp([&](const std::vector<uint8_t>& data) {
        crystal::parseRtcpCompound(data.data(), data.size(),
                                   [&](const crystal::RtcpPacket& p) {
            switch (p.kind) {
            case crystal::RtcpKind::Nack:
                // 对端请求重传：展开 PID+BLP 为 seq，逐个从重传缓冲补发
                for (uint16_t seq : crystal::NackPacket::expandEntries(p.nack.entries)) {
                    auto original = retxBuffer.get(seq, nowMs());
                    if (!original.empty()) {
                        pc->sendMedia(original);  // 原样补发缓存的 RTP bytes
                    }
                }
                break;
            case crystal::RtcpKind::Pli:
                // 对端画面不可恢复，下一帧强制 IDR
                encoder.forceKeyframe();
                crystal::Logger::info("RTCP: PLI received, forcing keyframe");
                break;
            case crystal::RtcpKind::ReceiverReport: {
                // 对端 RR → 更新我方两条发送流的 RTT/对端观测丢包率
                // （reporter 内部按 SSRC 匹配，各取所需）
                uint64_t ntp = crystal::nowNtp();
                videoSendReport.onReceiverReport(p.rr, ntp);
                audioSendReport.onReceiverReport(p.rr, ntp);
                break;
            }
            case crystal::RtcpKind::SenderReport: {
                // 对端 SR → 记录到达时刻（我方 RR 报告块 LSR/DLSR 基准）
                uint64_t ntp = crystal::nowNtp();
                videoRecvReport.onSenderReport(p.sr.ssrc, ntp);
                audioRecvReport.onSenderReport(p.sr.ssrc, ntp);
                break;
            }
            default:
                break;  // SDES 等本阶段不处理
            }
        });
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

    // ---- 回调3b：解码错误 → 节流后请求关键帧 ----
    // sendPli 内部 500ms 节流；错误隐藏帧（花屏）同样触发
    decoder.onError([&]() { sendPli(); });

    // ---- 回调4：视频编码输出 → RTP 打包 → 发送 ----
    // 编码器每输出一个 NAL Unit，打包为 RTP 包并通过 PeerConnection 发送
    // 同一帧的多个 NAL 共享同一 RTP 时间戳（videoRtpTs 在回调5按帧推进）
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, videoRtpTs);  // 大 NAL 自动 FU-A 分片
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();           // 序列化为字节流
            // ① 存入重传缓冲（对端 NACK 时按 seq 补发的就是这份 bytes）
            retxBuffer.store(pkt.sequenceNumber(), data, nowMs());
            // ② 发送侧统计（SR 的包数/字节数/最新时间戳）
            videoSendReport.onPacketSent(pkt.sequenceNumber(),
                                         pkt.payload().size(), pkt.timestamp());
            // ③ 实际发送
            pc->sendMedia(data);                   // 通过 P2P 连接发送
            videoSent = true;
        }
    });

    // ---- 回调5：视频采集 → 编码 ----
    // 摄像头每采集一帧 YUV 数据，送入编码器
    // 【RTP 时间戳】Phase 1 所有包 timestamp=0，接收端无法判断帧边界，
    // 也会破坏抖动计算。视频时钟 90kHz，每帧推进 90000/fps。
    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        videoRtpTs += 90000 / encConfig.fps;  // 一帧周期（30fps → 3000）
        encoder.encode(yuvData, len);         // encode 同步触发回调4
    });

    // ---- 回调6：音频采集 → 编码 → RTP 打包 → 发送 ----
    // 麦克风采集 PCM 数据，编码为 Opus，打包为 RTP 包并发送
    // 音频时钟 48kHz，每帧推进 = 每帧采样数（Opus @48kHz）
    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());  // PCM → Opus
        if (!opusFrame.empty()) {
            audioRtpTs += static_cast<uint32_t>(opusEncoder.frameSize());
            auto packets = audioPacketizer.packetizeOpus(opusFrame, audioRtpTs);  // Opus → RTP 包
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();           // 序列化为字节流
                audioSendReport.onPacketSent(pkt.sequenceNumber(),
                                             pkt.payload().size(), pkt.timestamp());
                pc->sendMedia(data);                   // 通过 P2P 连接发送
                audioSent = true;
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
    // 轮询 SDL 事件 + 驱动 RTCP 周期任务（NACK 重试 / SR-RR / 统计）
    // ====================================================================
    uint64_t lastReportMs = 0;
    uint64_t lastGivenUp = 0;
    while (g_running && !renderer.shouldQuit()) {
        renderer.pollEvents();  // 处理 SDL 窗口事件
        uint64_t now = nowMs();

        // --- NACK 重试驱动：到期未恢复的 seq 再次请求 ---
        sendNack(nackRequester.tick(now));

        // --- 重试耗尽 → PLI 兜底（参考帧链已断，重传救不回来）---
        if (nackRequester.givenUpCount() > lastGivenUp) {
            lastGivenUp = nackRequester.givenUpCount();
            sendPli();
        }

        // --- 每 5s：SR/RR + 统计行（RFC 3550 推荐周期）---
        if (now - lastReportMs >= 5000) {
            lastReportMs = now;
            uint64_t ntp = crystal::nowNtp();

            // 视频流：发过包 → SR(内嵌对端接收报告块)；只收未发 → 纯 RR
            crystal::ReportBlock blk;
            std::vector<crystal::ReportBlock> blocks;
            if (videoRecvReport.buildBlock(ntp, blk)) blocks.push_back(blk);
            if (videoSent) {
                pc->sendRtcp(videoSendReport.buildReport(ntp, blocks));
            } else if (!blocks.empty()) {
                crystal::ReceiverReport rr;
                rr.ssrc = videoSsrc;      // 报告发起方 SSRC
                rr.blocks = blocks;
                std::vector<uint8_t> buf;
                crystal::appendReceiverReport(buf, rr);
                crystal::SdesPacket sdes;
                sdes.ssrc = videoSsrc;
                sdes.cname = "crystal-video";
                crystal::appendSdes(buf, sdes);   // 复合包需含 SDES
                pc->sendRtcp(buf);
            }
            // 音频流：同理
            blocks.clear();
            if (audioRecvReport.buildBlock(ntp, blk)) blocks.push_back(blk);
            if (audioSent) {
                pc->sendRtcp(audioSendReport.buildReport(ntp, blocks));
            } else if (!blocks.empty()) {
                crystal::ReceiverReport rr;
                rr.ssrc = audioSsrc;
                rr.blocks = blocks;
                std::vector<uint8_t> buf;
                crystal::appendReceiverReport(buf, rr);
                crystal::SdesPacket sdes;
                sdes.ssrc = audioSsrc;
                sdes.cname = "crystal-audio";
                crystal::appendSdes(buf, sdes);
                pc->sendRtcp(buf);
            }

            // --- 终端质量统计行 ---
            std::string rtt = videoSendReport.hasRtt()
                                  ? std::to_string(
                                        static_cast<int>(videoSendReport.rttMs()))
                                  : "n/a";
            crystal::Logger::info(
                "[stats] 丢包 v{:.1f}% a{:.1f}% | 抖动 v{:.1f}ms a{:.1f}ms | "
                "RTT {}ms | 重传 {} miss {} | NACK {} 放弃 {}",
                videoRecvReport.lossRate() * 100, audioRecvReport.lossRate() * 100,
                videoRecvReport.jitterMs(), audioRecvReport.jitterMs(), rtt,
                retxBuffer.retransmittedCount(), retxBuffer.missCount(),
                nackRequester.requestedCount(), nackRequester.givenUpCount());
        }

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
