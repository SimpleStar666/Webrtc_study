// ============================================================================
// rtcp_reporter.cpp - SR/RR 统计器实现
// ============================================================================

#include "media/rtcp/rtcp_reporter.h"
#include <algorithm>

namespace crystal {

// ============================================================================
// SendSideReporter
// ============================================================================
SendSideReporter::SendSideReporter(uint32_t ssrc, std::string cname)
    : ssrc_(ssrc), cname_(std::move(cname)) {}

void SendSideReporter::onPacketSent(uint16_t /*seq*/, size_t payloadBytes,
                                    uint32_t rtpTs) {
    std::lock_guard<std::mutex> lock(mutex_);
    packetCount_++;
    octetCount_ += static_cast<uint32_t>(payloadBytes);
    lastRtpTs_ = rtpTs;
}

std::vector<uint8_t> SendSideReporter::buildReport(
    uint64_t nowNtp, const std::vector<ReportBlock>& blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    SenderReport sr;
    sr.ssrc = ssrc_;
    sr.ntpTimestamp = nowNtp;
    sr.rtpTimestamp = lastRtpTs_;
    sr.packetCount = packetCount_;
    sr.octetCount = octetCount_;
    sr.blocks = blocks;

    // RFC 3550 6.4.1：SR 之后必须跟 SDES（CNAME）
    std::vector<uint8_t> out;
    appendSenderReport(out, sr);
    SdesPacket sdes;
    sdes.ssrc = ssrc_;
    sdes.cname = cname_;
    appendSdes(out, sdes);
    return out;
}

void SendSideReporter::onReceiverReport(const ReceiverReport& rr,
                                        uint64_t nowNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& blk : rr.blocks) {
        if (blk.ssrc != ssrc_) continue;  // 只认针对本流的报告块
        remoteFractionLost_ = blk.fractionLost;
        if (blk.lsr != 0) {
            // RTT = A - LSR - DLSR（A = 本机当前 NTP，全部为中间 32 位格式，
            // 32 位无符号回绕减法天然正确）
            uint64_t nowMid = (nowNtp >> 16) & 0xFFFFFFFF;
            uint32_t rttMid = static_cast<uint32_t>(nowMid - blk.lsr - blk.dlsr);
            rttMs_ = rttMid / 65536.0 * 1000.0;
        }
    }
}

uint32_t SendSideReporter::ssrc() const { return ssrc_; }

bool SendSideReporter::hasRtt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rttMs_ >= 0;
}

double SendSideReporter::rttMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rttMs_;
}

uint8_t SendSideReporter::remoteFractionLost() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteFractionLost_;
}

// ============================================================================
// RecvSideReporter
// ============================================================================
RecvSideReporter::RecvSideReporter(uint32_t clockRate) : clockRate_(clockRate) {}

void RecvSideReporter::onPacketReceived(uint32_t ssrc, uint16_t seq,
                                        uint32_t rtpTs, uint64_t arrivalNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        // 首包：采纳 SSRC，初始化序列号基准（RFC 3550 A.1）
        ssrc_ = ssrc;
        baseSeq_ = seq;
        maxSeq_ = seq;
        cycles_ = 0;
        active_ = true;
    } else if (ssrc != ssrc_) {
        return;  // 非本流包（每实例只跟踪一条流）
    }

    // 更新最高序列号与回绕计数
    // int16_t 差值正确处理回绕（与 JitterBuffer 同技巧）
    int16_t delta = static_cast<int16_t>(seq - maxSeq_);
    if (delta > 0) {
        if (seq < maxSeq_) cycles_ += 65536;  // 正向跳变且值变小 → 回绕
        maxSeq_ = seq;
    } else if (static_cast<int16_t>(seq - baseSeq_) < 0) {
        // 乱序首包场景：比基准更早的包迟到（如首包 seq=2 后 seq=1 才到），
        // 必须下修 baseSeq_，否则期望包数少算、丢包被漏统计
        baseSeq_ = seq;
    }
    received_++;

    // 抖动更新（RFC 3550 A.8）：到达时刻从 NTP 换算到 RTP 单位
    // 用完整 64 位相乘再右移 32（避免中间 >>16 的截断误差累积）
    uint64_t arrivalRtp =
        (arrivalNtp * static_cast<uint64_t>(clockRate_)) >> 32;
    int64_t transit = static_cast<int64_t>(arrivalRtp) - static_cast<int64_t>(rtpTs);
    int64_t d = transit - lastTransit_;
    if (d < 0) d = -d;
    jitter_ += (d - jitter_) / 16.0;
    lastTransit_ = transit;
}

void RecvSideReporter::onSenderReport(uint32_t ssrc, uint64_t arrivalNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 只记录本流的 SR：未激活（尚不知道本流 SSRC）时不能记录，
    // 否则会把其他流的 SR 错误归到本流，导致 LSR/DLSR 计算错误
    if (active_ && ssrc == ssrc_) {
        lastSrNtp_ = arrivalNtp;  // 记录 LSR 基准（供报告块 DLSR 用）
    }
}

bool RecvSideReporter::buildBlock(uint64_t nowNtp, ReportBlock& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return false;

    // RFC 3550 A.1：期望包数与丢包
    uint32_t extMax = cycles_ + maxSeq_;
    uint64_t expected = extMax - baseSeq_ + 1;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    if (lost < 0) lost = 0;

    // 间隔丢包率（fraction lost，自上份报告以来的增量）
    int64_t expectedInterval = static_cast<int64_t>(expected - expectedPrior_);
    int64_t receivedInterval = static_cast<int64_t>(received_ - receivedPrior_);
    int64_t lostInterval = expectedInterval - receivedInterval;
    uint8_t fraction = 0;
    if (expectedInterval > 0 && lostInterval > 0) {
        fraction = static_cast<uint8_t>(
            std::min(255.0, lostInterval * 256.0 / expectedInterval));
    }
    expectedPrior_ = expected;
    receivedPrior_ = received_;

    out = ReportBlock{};
    out.ssrc = ssrc_;
    out.fractionLost = fraction;
    out.cumulativeLost = static_cast<uint32_t>(
        std::min<uint64_t>(0xFFFFFF, static_cast<uint64_t>(lost)));
    out.extHighestSeq = extMax;
    out.jitter = static_cast<uint32_t>(jitter_);
    // LSR/DLSR：对端最近一次 SR 的到达时刻
    if (lastSrNtp_ != 0) {
        out.lsr = ntpMiddle32(lastSrNtp_);
        uint64_t delayNtp = nowNtp - lastSrNtp_;
        out.dlsr = static_cast<uint32_t>((delayNtp >> 16) & 0xFFFFFFFF);
    }
    return true;
}

bool RecvSideReporter::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

uint32_t RecvSideReporter::remoteSsrc() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssrc_;
}

double RecvSideReporter::lossRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return 0.0;
    uint32_t extMax = cycles_ + maxSeq_;
    uint64_t expected = extMax - baseSeq_ + 1;
    if (expected == 0) return 0.0;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    if (lost <= 0) return 0.0;
    return static_cast<double>(lost) / static_cast<double>(expected);
}

double RecvSideReporter::jitterMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jitter_ / static_cast<double>(clockRate_) * 1000.0;
}

} // namespace crystal
