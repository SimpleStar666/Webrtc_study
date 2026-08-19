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
    // 只做最轻量的累加：本函数在发送热路径上，每个 RTP 包都调一次，
    // 不能有任何重活。seq 目前未用于统计（丢包由接收端算），故不保存。
    packetCount_++;
    octetCount_ += static_cast<uint32_t>(payloadBytes);
    // 记录最新 RTP 时间戳：SR 里要填"NTP 时刻 ↔ RTP 时刻"的换算锚点，
    // 对端用它做音画同步（lip sync）
    lastRtpTs_ = rtpTs;
}

// ============================================================================
// SendSideReporter::buildReport - 构造 [SR + SDES] 复合包
// ============================================================================
//
// 【产物字节流】一次调用产出两个 RTCP 子包背靠背拼在一起：
//   [SR 头部|SSRC|NTP时间戳|RTP时间戳|包数|字节数|报告块*] [SDES|CNAME]
//   由 parseRtcpCompound() 在对端逐个子包解析。
//
// 【报告块从哪来】参数 blocks 是调用方（main_client）用我方的
// RecvSideReporter 生成的——即"我发 SR 的同时也捎带我对对端的接收统计"，
// 一个复合包双向信息齐活，省一次独立 RR 的发送。
std::vector<uint8_t> SendSideReporter::buildReport(
    uint64_t nowNtp, const std::vector<ReportBlock>& blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    SenderReport sr;
    sr.ssrc = ssrc_;
    sr.ntpTimestamp = nowNtp;      // SR 的"发出时刻"，对端 LSR 的来源
    sr.rtpTimestamp = lastRtpTs_; // 此刻发出的最后一个包的 RTP 时间戳
    sr.packetCount = packetCount_;
    sr.octetCount = octetCount_;
    sr.blocks = blocks;

    // RFC 3550 6.4.1：SR 之后必须跟 SDES（CNAME）
    // 【为什么强制带 CNAME】CNAME 是"用户/会话"级标识（跨越多条流不变），
    // 对端靠它把同一人的音频流和视频流关联起来做音画同步
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
    // 一份 RR 可能带着针对多条流的报告块（音视频各一块），
    // 遍历后只认 ssrc == 本流的那块，其余的与"我"无关
    for (const auto& blk : rr.blocks) {
        if (blk.ssrc != ssrc_) continue;  // 只认针对本流的报告块
        // 对端眼里的我方丢包率（带宽估计的重要输入，本阶段仅展示）
        remoteFractionLost_ = blk.fractionLost;
        // LSR=0 表示对端从未收到过我方 SR（此时还算不出 RTT），跳过
        if (blk.lsr != 0) {
            // RTT = A - LSR - DLSR（A = 本机当前 NTP，全部为中间 32 位格式，
            // 32 位无符号回绕减法天然正确）
            //
            // 【完整时间线（数值示例）】
            //   t=10.000s  我方发出 SR（NTP 时间戳 T）
            //   t=10.045s  对端收到该 SR        ← 去程 45ms
            //   t=10.050s  对端发出 RR：
            //                LSR = T 的中间 32 位（回显 SR 的发出时刻）
            //                DLSR = 5ms（对端收 SR 到发 RR 的处理延迟）
            //   t=10.098s  我方收到 RR：A = T + 98ms
            //   RTT = A - LSR - DLSR = 98ms - 5ms = 93ms
            //         = 去程 45ms + 回程 48ms（纯网络往返，对端处理时间已被扣除）
            //
            // 【为什么全程不用两端时钟同步】LSR 是"我方时钟"打的戳经对端
            // 回显回来的，A 也是我方时钟的当前值——减法在同一时钟系内完成，
            // 对端时钟准不准完全不影响结果（这就是 LSR/DLSR 法的精妙之处）。
            uint64_t nowMid = (nowNtp >> 16) & 0xFFFFFFFF;
            uint32_t rttMid = static_cast<uint32_t>(nowMid - blk.lsr - blk.dlsr);
            // 中间 32 位格式：1 秒 = 65536，除以 65536 得秒，×1000 得毫秒
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
        // 【为什么从首包"学"SSRC 而不是配置传入】对端 SSRC 是随机生成的，
        // 本端事先无从知晓；RTP 包头里每个包都带 SSRC，首包到达时抄下来即可
        ssrc_ = ssrc;
        baseSeq_ = seq;   // 期望包数公式的基准："从 seq 开始数"
        maxSeq_ = seq;    // 当前见过的最大序列号
        cycles_ = 0;      // 序列号回绕计数（见下方）
        active_ = true;
    } else if (ssrc != ssrc_) {
        return;  // 非本流包（每实例只跟踪一条流）
    }

    // 更新最高序列号与回绕计数
    // int16_t 差值正确处理回绕（与 JitterBuffer 同技巧）
    //
    // 【回绕计数 cycles_ 的作用】16 位序列号 65535 之后回到 0。仅看裸值，
    // 0 比 65535"小"；加上 cycles_（每次回绕记 65536）拼成 32 位扩展序列号，
    // 0 + 65536 = 65536 > 65535，大小关系恢复正确。
    //
    // 【数值示例】maxSeq_=65530 时收到 seq=3：
    //   delta = (int16_t)(3 - 65530) = 9 → 正向
    //   3 < 65530 值变小但 delta>0 → 判定发生了回绕，cycles_ += 65536
    //   extMax = 65536 + 3 = 65539，期望包数 = 65539 - base + 1 连续无跳变
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
    //
    // 【抖动的直觉理解】理想网络里"包间隔 = 发包间隔"，transit 恒定；
    // 网络抖动让 transit 忽大忽小。取相邻两次 transit 的差值 d，
    // 指数平滑后就是 RFC 定义的抖动（jitter）。
    //
    // 【数值示例（90kHz 视频时钟）】
    //   包 i:   transit = 3020 ticks   包 i+1: transit = 2980 ticks
    //   d = |2980 - 3020| = 40 ticks
    //   若上次 jitter = 100: 新 jitter = 100 + (40-100)/16 = 96.25
    //   平滑系数 1/16：新值只占 1/16 权重，毛刺冲不垮长期估计
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
    // 流未激活（一个包都没收到过）→ 没有任何可报告的统计
    if (!active_) return false;

    // ---- 第一部分：累计丢包（RFC 3550 A.1）----
    //
    // 【期望包数公式】expected = 扩展最高序列号 - 首包序列号 + 1
    //   直觉："按发送顺序本应收到的包数"
    // 【累计丢包】lost = expected - received（实际收到的）
    //
    // 【数值示例】首包 base=100，最高 extMax=199，实收 received=95
    //   expected = 199 - 100 + 1 = 100
    //   lost = 100 - 95 = 5（累计丢了 5 个）
    uint32_t extMax = cycles_ + maxSeq_;
    uint64_t expected = extMax - baseSeq_ + 1;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    if (lost < 0) lost = 0;  // 乱序迟到包会让 received 反超，钳到 0

    // ---- 第二部分：间隔丢包率 fraction lost（8 位字段的由来）----
    //
    // 【为什么报告"增量"而不是"累计"丢包率】累计值反映的是整个会话的
    // 历史，网络恢复后它降不下来；增量值（自上份报告以来）才能反映
    // "当前网络怎么样"。带宽估计算法（如 GCC）正是吃这个增量值。
    //
    // 【数值示例】本周期新增期望 100 包、实收 97 包：
    //   lostInterval = 100 - 97 = 3
    //   fraction = 3 × 256 / 100 = 7.68 → 取整 7（约 2.7% 丢包率）
    //   8 位字段：256 级精度 × 每级 0.39%，对拥塞判断足够
    int64_t expectedInterval = static_cast<int64_t>(expected - expectedPrior_);
    int64_t receivedInterval = static_cast<int64_t>(received_ - receivedPrior_);
    int64_t lostInterval = expectedInterval - receivedInterval;
    uint8_t fraction = 0;
    if (expectedInterval > 0 && lostInterval > 0) {
        fraction = static_cast<uint8_t>(
            std::min(255.0, lostInterval * 256.0 / expectedInterval));
    }
    // 记住本周期值，下个周期用它算"自上份报告以来"的增量
    expectedPrior_ = expected;
    receivedPrior_ = received_;

    // ---- 第三部分：组装报告块 ----
    out = ReportBlock{};  // 先清零（保证未显式赋值的字段确定是 0）
    out.ssrc = ssrc_;     // 告诉对端"这块统计的是你的哪条流"
    out.fractionLost = fraction;
    out.cumulativeLost = static_cast<uint32_t>(
        std::min<uint64_t>(0xFFFFFF, static_cast<uint64_t>(lost)));  // 24 位上限
    out.extHighestSeq = extMax;  // 对端用它与自己发送记录比对，也能反推丢包
    out.jitter = static_cast<uint32_t>(jitter_);
    // LSR/DLSR：对端最近一次 SR 的到达时刻
    //
    // 【这两个字段配对的用途】对端收到本块后算 RTT：
    //   RTT = 对端当前NTP - LSR - DLSR（详见 SendSideReporter 的图解）
    if (lastSrNtp_ != 0) {
        out.lsr = ntpMiddle32(lastSrNtp_);
        // DLSR = 从"收到对端 SR"到"发出本报告"的处理延迟（1/65536 秒单位）
        // 换算：>>16 相当于把 64 位 NTP 差值压成中间 32 位格式
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

// 累计丢包率（0~1），[stats] 日志"丢包 vX%"的数据来源
// 公式与 buildBlock 第一部分相同：lost / expected
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

// 平滑抖动换算为毫秒：RTP ticks → 秒 → 毫秒
// 【为什么需要 clockRate】抖动内部以"RTP 时钟刻度"为单位，
// 视频 90kHz 与音频 48kHz 的一个 tick 时长不同，统一换算成毫秒才可比
double RecvSideReporter::jitterMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jitter_ / static_cast<double>(clockRate_) * 1000.0;
}

} // namespace crystal
