// ============================================================================
// rtcp_reporter.h - SR/RR 统计器（发送侧 + 接收侧）
// ============================================================================
//
// 【SendSideReporter —— 发送侧，每条发送流一个实例】
//   记录发包计数；周期构造 SR 复合包（SR + SDES）；
//   解析对端 RR 的 LSR/DLSR 计算 RTT：
//     RTT = 收到 RR 的 NTP 时刻 - LSR - DLSR   （RFC 3550 A.6）
//   【面试考点】RTT 只能在发送侧算：LSR 是"对端收到我方 SR"的时刻，
//   DLSR 是"对端从收 SR 到发 RR"的间隔，两者之差被 NTP 时钟对齐后
//   才能减出网络往返时间。
//
// 【RecvSideReporter —— 接收侧，每条接收流一个实例】
//   每个 RTP 包到达时更新丢包（RFC 3550 A.1）与抖动（A.8）统计；
//   周期产出 ReportBlock（可同时嵌入 RR 与我方 SR）。
//
// 【抖动算法（RFC 3550 A.8）】
//   transit = 到达时刻(RTP 单位) - RTP 时间戳
//   D = |transit_i - transit_{i-1}|
//   jitter += (D - jitter) / 16
//   除以 16 相当于指数平滑（增益 1/16），既跟得上波动又抗毛刺。
// ============================================================================

#pragma once

#include "media/rtcp/rtcp_packet.h"
#include <mutex>
#include <string>
#include <vector>

namespace crystal {

// ============================================================================
// SendSideReporter
// ============================================================================
class SendSideReporter {
public:
    SendSideReporter(uint32_t ssrc, std::string cname);

    // 每发出一个 RTP 包调用（payloadBytes = 负载字节数，不含 RTP 头）
    void onPacketSent(uint16_t seq, size_t payloadBytes, uint32_t rtpTs);

    // 构造 SR 复合包（SR + SDES），blocks 为我方对对端流的接收统计
    std::vector<uint8_t> buildReport(uint64_t nowNtp,
                                     const std::vector<ReportBlock>& blocks);

    // 收到对端 RR 时调用（自动匹配 blocks 中 ssrc == 本流的报告块）
    void onReceiverReport(const ReceiverReport& rr, uint64_t nowNtp);

    uint32_t ssrc() const;
    bool hasRtt() const;       // 是否已算出 RTT
    double rttMs() const;      // RTT（毫秒）
    uint8_t remoteFractionLost() const;  // 对端观测的我方丢包率（256=100%）

private:
    uint32_t ssrc_;
    std::string cname_;
    mutable std::mutex mutex_;
    uint32_t packetCount_ = 0;
    uint32_t octetCount_ = 0;
    uint32_t lastRtpTs_ = 0;
    double rttMs_ = -1;
    uint8_t remoteFractionLost_ = 0;
};

// ============================================================================
// RecvSideReporter
// ============================================================================
class RecvSideReporter {
public:
    // clockRate - 该流的 RTP 时钟（视频 90000 / 音频 48000），抖动换算用
    explicit RecvSideReporter(uint32_t clockRate);

    // 每个 RTP 包到达时调用；首个包自动采纳该流的 SSRC
    void onPacketReceived(uint32_t ssrc, uint16_t seq, uint32_t rtpTs,
                          uint64_t arrivalNtp);

    // 收到对端 SR 时调用（记录 LSR 基准时刻）
    void onSenderReport(uint32_t ssrc, uint64_t arrivalNtp);

    // 产出报告块（嵌入 RR 或 SR）；流未激活时返回 false
    bool buildBlock(uint64_t nowNtp, ReportBlock& out);

    bool active() const;           // 是否已收到过包
    uint32_t remoteSsrc() const;   // 对端流 SSRC
    double lossRate() const;       // 累计丢包率 [0,1]
    double jitterMs() const;       // 平滑抖动（毫秒）

private:
    uint32_t clockRate_;
    mutable std::mutex mutex_;
    uint32_t ssrc_ = 0;
    bool active_ = false;
    uint16_t baseSeq_ = 0;       // 首包序列号（期望包数基准）
    uint16_t maxSeq_ = 0;        // 最高序列号
    uint32_t cycles_ = 0;        // 回绕计数（<<16 进扩展序列号）
    uint64_t received_ = 0;
    uint64_t expectedPrior_ = 0; // 上份报告的期望包数（fraction 增量用）
    uint64_t receivedPrior_ = 0; // 上份报告的接收包数
    double jitter_ = 0;          // 平滑抖动（RTP 时间单位）
    int64_t lastTransit_ = 0;
    uint64_t lastSrNtp_ = 0;     // 最近收到对端 SR 的 NTP 时刻
};

} // namespace crystal
