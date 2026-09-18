// ============================================================================
// twcc_recorder.cpp - TWCC 到达时刻记录器实现
// ============================================================================
//
// 【窗口生命周期】
//   首包到达 → 开窗（记基准 seq/时刻）→ 持续记录 → buildFeedback 判定
//   （≥100ms 或 ≥64 包）→ 产出 TwccFeedback（缺口补 lost）→ 关窗清空
//
// 【缺口补 lost】
//   窗口覆盖 [窗口首包 seq, 最大 seq] 整段：收到样本之外的位置全部标记
//   丢失。这正是 TWCC 的丢包检测来源——比 RR 的累计 fraction lost 更细
//   （精确到每个包、每个 100ms 窗口）。
//
#include "media/rtcp/twcc_recorder.h"
#include <algorithm>

namespace crystal {

namespace {
constexpr double kWindowMs = 100.0;      // 反馈窗口时长
constexpr size_t kMaxBacklog = 64;       // 积压包数上限（防反馈自身占用带宽）
constexpr size_t kMaxSpread = 512;       // 窗口最大序号跨度（防畸形跳变撑爆 symbol 表）
constexpr double kEpochLimitMs = 240000.0; // 会话时钟重置线（24 位 refTime 上限≈262s，留余量）
} // namespace

TwccRecorder::TwccRecorder(uint32_t senderSsrc, uint32_t mediaSsrc)
    : senderSsrc_(senderSsrc), mediaSsrc_(mediaSsrc) {}

void TwccRecorder::setMediaSsrc(uint32_t ssrc) {
    mediaSsrc_ = ssrc;
}

void TwccRecorder::onPacket(uint16_t twccSeq, double arrivalMs) {
    // 会话时钟原点只在首个历史包时设定
    if (!hasEpoch_) {
        epochBaseMs_ = arrivalMs;
        hasEpoch_ = true;
    }

    // 开窗：记录基准序号与窗口起点
    if (pending_.empty()) {
        windowBaseSeq_ = twccSeq;
        windowStartMs_ = arrivalMs;
    }

    // offset = uint16(seq - base)：回绕安全（65535 → 0 连续）
    // 超出最大跨度的迟到包直接丢弃（比窗口老 512 个序号，无统计价值）
    size_t off = static_cast<uint16_t>(twccSeq - windowBaseSeq_);
    if (off >= kMaxSpread) return;

    pending_[off] = arrivalMs;  // 重复 seq（重传后重复投递）后者胜
}

bool TwccRecorder::buildFeedback(double nowMs, TwccFeedback& out) {
    if (pending_.empty()) return false;

    // 窗口判定：时长够 或 积压够
    if (nowMs - windowStartMs_ < kWindowMs && pending_.size() < kMaxBacklog)
        return false;

    // 会话时钟逼近 24 位编码上限 → 以本窗口首样本重置原点。
    // 重置瞬间发送端还原的 x 轴会跳一次，trendline 窗口（20 样本）很快
    // 滚过，3 轮过载去抖可吸收（简化设计，生产做法是模 2^24 差分补跳）
    double firstArrival = pending_.begin()->second;
    if (firstArrival - epochBaseMs_ > kEpochLimitMs)
        epochBaseMs_ = firstArrival;

    // ---- 产出 feedback ----
    out.senderSsrc = senderSsrc_;
    out.mediaSsrc = mediaSsrc_;
    out.baseSeq = windowBaseSeq_;
    out.refTimeMs = firstArrival - epochBaseMs_;  // 相对会话原点（编码不饱和）

    out.received.clear();
    out.lost.clear();
    size_t count = pending_.rbegin()->first + 1;  // 窗口 = 最大偏移 + 1
    for (size_t off = 0; off < count; ++off) {
        uint16_t seq = static_cast<uint16_t>(windowBaseSeq_ + off);  // 回绕安全
        auto it = pending_.find(off);
        if (it != pending_.end())
            out.received.push_back({seq, it->second - epochBaseMs_});
        else
            out.lost.push_back(seq);  // 缺口 = 丢失
    }

    // ---- 关窗 ----
    pending_.clear();
    return true;
}

} // namespace crystal
