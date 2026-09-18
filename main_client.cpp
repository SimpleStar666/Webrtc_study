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
//   - TwccRecorder     : TWCC 到达记录器，收集每包到达时刻回传对端
//   - GccController    : GCC 带宽估计器，融合丢包/延迟趋势做 AIMD 调码率
//
// 【模块协作关系 - 完整数据流】
//
//   ┌──────────────── 发送链路（本地→远端，三级线程解耦 Phase D）──┐
//   │                                                              │
//   │  采集线程: 摄像头 → [V4L2Capture] → memcpy 入队             │
//   │  帧队列  : [SpscRing 容量2，无锁，满丢最旧]                 │
//   │  编码线程: 取最新帧 → [H264Encoder] → NAL Unit             │
//   │     → [RtpPacketizer] → RTP包                                │
//   │     → [TransportManager/PeerConnection] → 网络               │
//   │                                                              │
//   │  采集线程: 麦克风 → [AlsaCapture] → PCM                       │
//   │     → [OpusEncoder] → Opus帧（码率/FEC 由原子交接的参数驱动）│
//   │     → [RtpPacketizer] → RTP包                                │
//   │     → [TransportManager/PeerConnection] → 网络               │
//   └──────────────────────────────────────────────────────────────┘
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
#include "media/rtcp/twcc_recorder.h"        // 工程化升级 v2：GCC 接收侧
#include "media/gcc/gcc_controller.h"        // 工程化升级 v2：GCC 发送侧
#include "media/monitor/metrics_collector.h"  // 工程化升级 v2：可观测性
#include "utils/spsc_ring.h"                 // 工程化升级 v2：无锁帧队列（Phase D）
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <random>
#include <chrono>
#include <map>
#include <mutex>

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
    // enableTwcc=true：每个包打传输层序号扩展头——GCC 带宽估计数据源
    crystal::RtpPacketizer videoPacketizer(96, 90000, videoSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF),
                                           1200, /*enableTwcc=*/true);
    // 音频打包器：PT=97（Opus），时钟=48000Hz
    crystal::RtpPacketizer audioPacketizer(97, 48000, audioSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF));
    // 解包器：从 RTP 包中提取 H.264 NAL 或 Opus 帧
    crystal::RtpDepacketizer depacketizer;
    // 视频抖动缓冲区：解决乱序和抖动，并检测丢失 seq（NACK 数据源）
    crystal::JitterBuffer videoJitterBuf(40);
    // 音频抖动缓冲区
    crystal::JitterBuffer audioJitterBuf(40);
    // 音频播放序号记忆（工程化升级 v2 新增，FEC 跳变检测用）：
    // consume 输出的包序号出现跳变 = 中间有帧丢失 → 触发 decodeFec 恢复。
    // 放在 audioJitterBuf 旁边是因为它俩同属"接收链路状态"
    bool hasLastAudioSeq = false;
    uint16_t lastAudioSeq = 0;

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
    // atomic：写方分别是编码线程（视频）/采集线程（音频），读方是主循环
    std::atomic<bool> videoSent{false}, audioSent{false};
    // 当前帧的 RTP 时间戳（修复 Phase 1 全 0 时间戳问题，见回调5）
    // videoRtpTs 仅编码线程推进/读取；audioRtpTs 仅音频采集线程访问
    uint32_t videoRtpTs = 0, audioRtpTs = 0;

    // ---- 步骤4：创建音视频编解码器 ----
    // 视频编码器：将 YUV 帧编码为 H.264 NAL Unit
    crystal::H264EncoderConfig encConfig;
    crystal::H264Encoder encoder(encConfig);

    // ---- 步骤4b：GCC 带宽估计（工程化升级 v2 新增）----
    // 发送侧状态：
    //   twccSendTimes_ 记录每个 TWCC 序号的发送时刻——feedback 回来后
    //   owd = 到达时刻 - 发送时刻（两端时钟偏移在差分中消掉）
    //   容量上限 512 条滚动淘汰（feedback 窗口 100ms，历史样本足够）
    std::map<uint16_t, double> twccSendTimes_;
    // twccSendTimes_ 的锁：编码线程写（onEncoded ⑤）/ RTCP 线程读写
    // （TransportFeedback 处理时查表算 OWD）——std::map 非线程安全
    std::mutex twccSendTimesMutex;
    // GCC 控制器：起始码率取编码器配置，闭环收敛到网络可用带宽
    crystal::GccController gcc(static_cast<uint32_t>(encConfig.bitrateKbps));
    // 码率原子交接（Phase D）：主循环 tick 后只 store，编码线程每帧
    // 前 load+应用——FFmpeg 编码上下文（encode/setBitrate）从此只被
    // 编码线程触碰，跨线程并发彻底消除
    std::atomic<uint32_t> gccTargetKbps_{static_cast<uint32_t>(encConfig.bitrateKbps)};
    uint32_t lastAppliedKbps = static_cast<uint32_t>(encConfig.bitrateKbps);  // 去重

    // ---- 视频发送三级解耦（Phase D 新增）----
    //   采集线程: memcpy 入队（回调只做拷贝，绝不碰编码器）
    //   帧队列  : SpscRing 无锁传递，容量 2，积压只留最新帧（满丢最旧，
    //             丢帧计数可观测——消费侧 drop 才安全，见 spsc_ring.h）
    //   编码线程: 取最新帧 → 应用码率 → encode → RTP 打包 → 发送
    // 为什么解耦：v1 里采集回调同步做 encode，一次编码耗时 > 帧周期时
    // 采集线程被拖住，V4L2 内核缓冲积压 → 采集延迟持续爬升
    crystal::SpscRing<std::vector<uint8_t>> frameQueue(2);
    std::atomic<bool> encodeRunning{true};   // 编码线程退出标志
    // Opus FEC 冗余度的原子交接：主循环 store（RR 实测丢包率），
    // 音频采集线程每帧前 load+应用——libopus 编码器状态不能并发 ctl
    std::atomic<uint32_t> opusLossPct_{0};
    // 接收侧状态：TWCC 到达记录器（feedback 数据源；mediaSsrc 收到对端首包后补）
    crystal::TwccRecorder twccRecorder(videoSsrc, 0);

    // ---- 可观测性指标（工程化升级 v2 新增）----
    // 每条流一个采集器：视频统计帧率/卡顿/E2E/码率，音频只统计 E2E/码率
    // （音频没有"帧率"概念，expectedFps=0 关闭帧率/卡顿段）
    crystal::MetricsCollector videoMetrics(90000, encConfig.fps);
    crystal::MetricsCollector audioMetrics(48000, 0);
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
        // 打点到达时刻（Phase D）：时间驱动 JitterBuffer 的时钟由调用方
        // 注入，类内不读系统时钟——同一时钟也喂给 metrics/RTCP，全链路一致
        pkt.setArrivalMs(nowMs());

        if (pkt.payloadType() == 96) {
            // === 接收统计（RR 数据源）：丢包/回绕/抖动 ===
            videoRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            // === GCC 趋势通道数据源：记录 TWCC 包到达时刻（工程化升级 v2）===
            // 只记时刻，不参与重排/解码——feedback 窗口 100ms 后整批回传对端
            uint16_t twccSeq;
            if (pkt.getTwccSeq(twccSeq))
                twccRecorder.onPacket(twccSeq, static_cast<double>(nowMs()));
            // === 体验指标打点：E2E 采样（工程化升级 v2）===
            videoMetrics.onPacketArrival(nowMs(), pkt.timestamp());
            // === NACK 状态机联动 ===
            // 任意到达的包（含重传包）都解除对应 seq 的待请求状态
            nackRequester.onReceived(pkt.sequenceNumber());
            // insert 返回本次新检测到的丢失 seq（≤64 的小间隙才有值）
            auto missing = videoJitterBuf.insert(pkt);
            // 状态机过滤出"现在就该请求"的 seq（其余进入重试队列）
            auto toRequest = nackRequester.onMissing(missing, nowMs());
            sendNack(toRequest);

            auto packets = videoJitterBuf.consume(nowMs());  // 时间驱动释放（Phase D）
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);  // RTP 解包为 NAL Unit
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());    // NAL 送入解码器
                }
            }
        } else if (pkt.payloadType() == 97) {
            // 音频不做 NACK（重传到达已错过播放时刻），
            // 抗丢包走 FEC+PLC 两层防线（工程化升级 v2 完成闭环）
            audioRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            audioMetrics.onPacketArrival(nowMs(), pkt.timestamp());  // E2E（v2）
            // 【bug 修复】此前音频分支只 consume 不 insert——包从未真正
            // 进入 JitterBuffer，远端音频从未被解码播放。insert 是包进入
            // 缓冲区的唯一入口，consume 只负责按序取出
            audioJitterBuf.insert(pkt);
            auto packets = audioJitterBuf.consume(nowMs());  // 时间驱动释放（Phase D）
            for (const auto& p : packets) {
                // ---- FEC 恢复：播放序号跳变 = 上一帧丢失（工程化升级 v2）----
                // 收到 B 时发现 A(seq-1) 没到 → decodeFec(B) 从 B 中提取
                // A 的内嵌冗余副本，恢复出 A 先播放（int16_t 差值比较
                // 天然处理 seq 回绕）；decodeFec 返回空则由 opusDecoder
                // 内部的 PLC 机制兜底
                if (hasLastAudioSeq &&
                    static_cast<int16_t>(p.sequenceNumber() - lastAudioSeq) != 1) {
                    auto recovered = opusDecoder.decodeFec(
                        p.payload().data(), p.payload().size());
                    if (!recovered.empty()) {
                        audioPlayer.play(recovered.data(), recovered.size());
                    }
                }
                // ---- 正常解码播放当前帧 ----
                auto frames = depacketizer.depacketizeOpus(p);  // RTP 解包为 Opus 帧
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());  // Opus 解码为 PCM
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());  // 播放 PCM 音频
                    }
                }
                hasLastAudioSeq = true;
                lastAudioSeq = p.sequenceNumber();
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
                // GCC 丢包通道：对端 RR 报告块里我方视频流的 fraction lost
                // 是"我方视频在对端眼里"的丢包率（256=100%）。
                // 只在本 RR 确实携带视频块时喂——避免音频 RR 触发重复的 AIMD 动作
                for (const auto& b : p.rr.blocks) {
                    if (b.ssrc == videoSsrc) {
                        gcc.onLossUpdate(b.fractionLost / 255.0);
                        break;
                    }
                }
                break;
            }
            case crystal::RtcpKind::TransportFeedback: {
                // 对端 TWCC 反馈 → (seq, arrival) → 查本地发送时刻还原 OWD
                // → GCC 趋势通道（工程化升级 v2）
                // 加锁：twccSendTimes_ 的写方在编码线程（onEncoded ②），
                // 这里在 RTCP 线程并发读——std::map 非线程安全
                crystal::FeedbackSample s;
                s.arrivals.reserve(p.twcc.received.size());
                s.owdMs.reserve(p.twcc.received.size());
                {
                    std::lock_guard<std::mutex> lock(twccSendTimesMutex);
                    for (const auto& a : p.twcc.received) {
                        auto it = twccSendTimes_.find(a.seq);
                        if (it == twccSendTimes_.end()) continue;  // 已淘汰/非本端发出
                        s.arrivals.push_back(a);
                        // OWD = 到达 - 发送。两端时钟不同步没关系：
                        // 恒定偏移在 GccController 的差分中被消掉
                        s.owdMs.push_back(a.arrivalMs - it->second);
                    }
                }
                if (!s.arrivals.empty()) gcc.onFeedback(s);
                break;
            }
            case crystal::RtcpKind::SenderReport: {
                    // 对端 SR → 记录到达时刻（我方 RR 报告块 LSR/DLSR 基准）
                    uint64_t ntp = crystal::nowNtp();
                    videoRecvReport.onSenderReport(p.sr.ssrc, ntp);
                    audioRecvReport.onSenderReport(p.sr.ssrc, ntp);
                    // 工程化升级 v2：SR 同时携带 NTP↔RTP 锚点（同一时刻
                    // 的两种表示），按 SSRC 喂给对应流的 MetricsCollector，
                    // 供 E2E 延迟计算（详见 metrics_collector.cpp 注释推导）
                    if (p.sr.ssrc == videoRecvReport.remoteSsrc()) {
                        videoMetrics.onSenderReportMapping(p.sr.rtpTimestamp,
                                                           nowMs());
                    } else if (p.sr.ssrc == audioRecvReport.remoteSsrc()) {
                        audioMetrics.onSenderReportMapping(p.sr.rtpTimestamp,
                                                           nowMs());
                    }
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
        videoMetrics.onRenderedFrame(nowMs());  // 工程化升级 v2：卡顿/帧率打点
    });

    // ---- 回调3b：解码错误 → 节流后请求关键帧 ----
    // sendPli 内部 500ms 节流；错误隐藏帧（花屏）同样触发
    decoder.onError([&]() { sendPli(); });

    // ---- 回调4：视频编码输出 → RTP 打包 → 发送 ----
    // 编码器每输出一个 NAL Unit，打包为 RTP 包并通过 PeerConnection 发送
    // 同一帧的多个 NAL 共享同一 RTP 时间戳（videoRtpTs 在编码线程按帧推进）
    // 【线程归属（Phase D）】本回调在编码线程执行（由 encode() 同步触发）
    //   依赖项均为安全：videoPacketizer 编码线程独占；retxBuffer/
    //   videoMetrics/videoSendReport 自带锁；sendMedia 库内线程安全；
    //   twccSendTimes_ 显式加锁（RTCP 线程并发读）
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, videoRtpTs);  // 大 NAL 自动 FU-A 分片
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();           // 序列化为字节流
            // ① 存入重传缓冲（对端 NACK 时按 seq 补发的就是这份 bytes）
            retxBuffer.store(pkt.sequenceNumber(), data, nowMs());
            // ② GCC：记录 TWCC 序号的发送时刻（feedback 回来后算 OWD 用）
            // 加锁：RTCP 线程处理 TransportFeedback 时并发读这张表
            if (pkt.hasTwcc()) {
                std::lock_guard<std::mutex> lock(twccSendTimesMutex);
                twccSendTimes_[pkt.twccSeq()] = static_cast<double>(nowMs());
                if (twccSendTimes_.size() > 512)   // 滚动淘汰最老样本
                    twccSendTimes_.erase(twccSendTimes_.begin());
            }
            // ③ 体验指标打点：发送码率（工程化升级 v2）
            videoMetrics.onBytesSent(nowMs(), data.size());
            // ④ 发送侧统计（SR 的包数/字节数/最新时间戳）
            videoSendReport.onPacketSent(pkt.sequenceNumber(),
                                         pkt.payload().size(), pkt.timestamp());
            // ⑤ 实际发送
            pc->sendMedia(data);                   // 通过 P2P 连接发送
            videoSent = true;
        }
    });

    // ---- 回调5：视频采集 → 帧队列（Phase D 三级解耦第一级）----
    // 【线程归属】本回调在 V4L2 采集线程执行——只做拷贝入队：
    //   · 绝不碰编码器（FFmpeg 上下文归编码线程独占）
    //   · 无锁 pushOne，队列满（编码落后 >2 帧）时最新帧被丢弃并计数
    // 编码耗时再长也拖不住采集线程，V4L2 内核缓冲不再积压——这是
    // 解耦的核心收益：采集节奏与编码速度彻底脱钩
    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        std::vector<uint8_t> frame(yuvData, yuvData + len);  // memcpy 快照
        frameQueue.pushOne(std::move(frame));               // 满则丢最新并计数
    });

    // ---- 回调6：音频采集 → 编码 → RTP 打包 → 发送 ----
    // 麦克风采集 PCM 数据，编码为 Opus，打包为 RTP 包并发送
    // 音频时钟 48kHz，每帧推进 = 每帧采样数（Opus @48kHz）
    // 【线程归属】本回调在 ALSA 采集线程执行。libopus 编码器状态不能
    // 并发 ctl：主循环只 store opusLossPct_（原子交接），实际
    // setPacketLossPct 由本线程每帧前应用（去重，值没变不重设）
    uint32_t lastAppliedLossPct = 0;  // 本线程的去重基准（仅采集线程访问）
    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        uint32_t lossPct = opusLossPct_.load(std::memory_order_relaxed);
        if (lossPct != lastAppliedLossPct) {          // 值变了才动 libopus
            lastAppliedLossPct = lossPct;
            opusEncoder.setPacketLossPct(lossPct);
        }
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());  // PCM → Opus
        if (!opusFrame.empty()) {
            audioRtpTs += static_cast<uint32_t>(opusEncoder.frameSize());
            auto packets = audioPacketizer.packetizeOpus(opusFrame, audioRtpTs);  // Opus → RTP 包
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();           // 序列化为字节流
                audioSendReport.onPacketSent(pkt.sequenceNumber(),
                                             pkt.payload().size(), pkt.timestamp());
                audioMetrics.onBytesSent(nowMs(), data.size());  // 体验指标（v2）
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

    // ====================================================================
    // 步骤9b：启动编码线程（Phase D 三级解耦第三级）
    // 取最新帧 → 应用 GCC 码率 → encode →（回调4）打包 → 发送
    // ====================================================================
    std::thread encodeThread([&]() {
        uint32_t appliedKbps = 0;  // 本线程码率去重基准（仅编码线程访问）
        uint64_t lastPushDrop = 0, lastConsDrop = 0;  // 丢帧计数快照
        while (encodeRunning.load(std::memory_order_relaxed)) {
            // 满丢最旧：积压 >1 帧时丢旧帧只留最新。实时视频永远编
            // "现在"——旧帧编出来到端上也错过渲染时刻，白占带宽。
            // （丢最旧必须在消费侧执行，原因见 spsc_ring.h 头注释）
            size_t backlog = frameQueue.size();
            if (backlog > 1) frameQueue.drop(backlog - 1);

            std::vector<uint8_t> frame;
            if (!frameQueue.popOne(frame)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;  // 队列空：轻量休眠等下一帧
            }

            // 码率原子交接的接收端：主循环只 store，本线程串行应用——
            // FFmpeg 上下文（encode/setBitrate）自此单线程独占
            uint32_t target = gccTargetKbps_.load(std::memory_order_relaxed);
            if (target != appliedKbps) {
                appliedKbps = target;
                encoder.setBitrate(target);
            }

            // RTP 时间戳按"实际经过的帧数"推进（含被丢的）：90kHz 时钟
            // 与真实采集节奏对齐——丢帧表现为画面跳一格，而非加速播放
            uint64_t pushDrop = frameQueue.pushDropCount();
            uint64_t consDrop = frameQueue.consumerDropCount();
            uint32_t skipped = static_cast<uint32_t>((pushDrop - lastPushDrop) +
                                                     (consDrop - lastConsDrop));
            lastPushDrop = pushDrop;
            lastConsDrop = consDrop;
            videoRtpTs += (1 + skipped) * (90000 / encConfig.fps);

            encoder.encode(frame.data(), frame.size());  // 同步触发回调4
        }
    });

    crystal::Logger::info("CrystalRTC client running. Press Ctrl+C to quit.");

    // ====================================================================
    // 步骤10：主循环
    // 轮询 SDL 事件 + 驱动 RTCP 周期任务（NACK 重试 / SR-RR / 统计）
    // ====================================================================
    uint64_t lastReportMs = 0;
    uint64_t lastGivenUp = 0;
    uint64_t lastTwccMs = 0;   // TWCC feedback 发送节拍（接收侧，100ms）
    uint64_t lastGccMs = 0;   // GCC tick 节拍（发送侧，100ms）
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

        // --- 每 100ms：接收侧发 TWCC feedback（GCC 趋势通道数据源）---
        if (now - lastTwccMs >= 100) {
            lastTwccMs = now;
            // 对端视频 SSRC 确认后写进 feedback 的 mediaSsrc 字段
            if (videoRecvReport.active())
                twccRecorder.setMediaSsrc(videoRecvReport.remoteSsrc());
            crystal::TwccFeedback fb;
            if (twccRecorder.buildFeedback(static_cast<double>(now), fb))
                pc->sendRtcp(crystal::appendTransportFeedback(fb));
        }

        // --- 每 100ms：GCC tick → 码率原子交接给编码线程（发送侧闭环）---
        // 主循环只 store 发布，不直接调 encoder.setBitrate——FFmpeg
        // 上下文归编码线程独占（Phase D 线程安全修复）
        if (now - lastGccMs >= 100) {
            lastGccMs = now;
            gcc.tick();
            uint32_t target = gcc.targetBitrateKbps();
            if (target != lastAppliedKbps) {  // 去重：码率没变不发布
                lastAppliedKbps = target;
                gccTargetKbps_.store(target, std::memory_order_relaxed);
                crystal::Logger::info("[gcc] 目标码率 → {}kbps", target);
            }
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
                "RTT {}ms | 重传 {} miss {} | NACK {} 放弃 {} | 编码丢帧 {} | "
                "GCC {}kbps slope {:+.3f}",
                videoRecvReport.lossRate() * 100, audioRecvReport.lossRate() * 100,
                videoRecvReport.jitterMs(), audioRecvReport.jitterMs(), rtt,
                retxBuffer.retransmittedCount(), retxBuffer.missCount(),
                nackRequester.requestedCount(), nackRequester.givenUpCount(),
                frameQueue.pushDropCount() + frameQueue.consumerDropCount(),
                lastAppliedKbps, gcc.trendSlope());

            // --- 体验侧指标行（工程化升级 v2 新增）---
            // 网络好 ≠ 体验好：[stats] 看链路，[metrics] 看用户感知
            if (videoSendReport.hasRtt())
                videoMetrics.setRttMs(videoSendReport.rttMs());
            if (audioSendReport.hasRtt())
                audioMetrics.setRttMs(audioSendReport.rttMs());
            crystal::Logger::info("[metrics][v] {}",
                                  videoMetrics.summaryLine());
            // 播放缓冲观测（Phase D）：水位稳态 ~160ms；下溢>0 说明
            // 供数跟不上播放（网络抖动/JB 耗尽），回落丢是水位收敛动作
            crystal::Logger::info("[metrics][a] {} | 播放缓冲 {}ms 下溢{} 回落丢{}ms",
                                  audioMetrics.summaryLine(),
                                  audioPlayer.bufferedMs(),
                                  audioPlayer.underflowCount(),
                                  audioPlayer.droppedMs());

            // --- 闭环：RR 实测丢包率 → 动态调 FEC 冗余度（工程化升级 v2）---
            // 对端 RR 报告块里我方音频流的 fraction lost，含义是
            // "我方音频流在对端眼里"的丢包率（8 位定点，255=100%）。
            // 换算成百分比喂给编码器，libopus 据此决定 FEC 冗余量：
            // 丢包率 2% 编 30% 冗余是浪费，丢包率 30% 编 2% 冗余等于没编
            // 原子交接（Phase D）：主循环只 store，音频采集线程每帧前
            // 应用——libopus 编码器状态不能被两个线程并发触碰
            opusLossPct_.store(
                audioSendReport.remoteFractionLost() * 100 / 255,
                std::memory_order_relaxed);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));  // ~60fps 轮询
    }

    // ====================================================================
    // 优雅退出：按逆序停止各模块
    // ====================================================================
    crystal::Logger::info("Shutting down...");
    capture.stopCapture();          // 停止视频采集（不再有新帧入队）
    alsaCapture.stopCapture();      // 停止音频采集
    // 收编码线程（Phase D）：必须在 encoder 析构前 join——线程内还在
    // 用 encoder/videoPacketizer 等栈对象，不 join 直接返回 = 析构竞态
    encodeRunning = false;
    if (encodeThread.joinable()) encodeThread.join();
    signalingClient.leaveRoom();    // 离开房间，通知信令服务器
    signalingClient.disconnect();   // 断开信令连接

    return 0;
}
