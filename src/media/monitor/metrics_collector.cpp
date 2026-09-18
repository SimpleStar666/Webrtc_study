// ============================================================================
// metrics_collector.cpp - 可观测性指标采集器实现（工程化升级 v2 新增）
// ============================================================================

#include "media/monitor/metrics_collector.h"
#include <algorithm>
#include <cstdio>

namespace crystal {

MetricsCollector::MetricsCollector(uint32_t clockRate, uint32_t expectedFps)
    : clockRate_(clockRate), expectedFps_(expectedFps) {}

// ----------------------------------------------------------------------------
// onRenderedFrame - 渲染打点：卡顿检测 + 帧率窗口
// ----------------------------------------------------------------------------
void MetricsCollector::onRenderedFrame(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // --- 卡顿检测：帧间隔超过 1.5 倍预期帧距即计一次 ---
    // 30fps 预期帧距 33.3ms，阈值 50ms；阈值放宽 1.5 倍是为了
    // 不把偶发的调度毛刺（一次缺页、一次日志刷盘）算成卡顿，只抓真掉帧。
    if (hasLastRender_ && expectedFps_ > 0) {
        uint64_t interval = nowMs - lastRenderMs_;
        uint64_t thresholdMs = 1500 / expectedFps_;  // 1.5 × (1000/fps)
        if (interval > thresholdMs) ++stallCount_;
    }
    hasLastRender_ = true;
    lastRenderMs_ = nowMs;
    renderTsMs_.push_back(nowMs);
    evictWindowsLocked(nowMs);
}

// ----------------------------------------------------------------------------
// onSenderReportMapping - 记录 SR 的 NTP↔RTP 锚点（E2E 计算基准）
// ----------------------------------------------------------------------------
// SR 同时携带"同一个时刻的两种表示"：对端 NTP 时间戳 + RTP 时间戳。
// 这一对就是钟表换算的锚点——有了它，任意 RTP 时间戳都能映射回
// 对端时钟的发送时刻，E2E 公式的核心依赖。
void MetricsCollector::onSenderReportMapping(uint32_t srRtpTs,
                                             uint64_t srArrivalMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    hasSrAnchor_ = true;
    srRtpTs_ = srRtpTs;
    srArrivalMs_ = srArrivalMs;
}

// ----------------------------------------------------------------------------
// onPacketArrival - E2E 采样（本项目 v2 最有面试价值的算法之一）
// ----------------------------------------------------------------------------
// 【问题】两台机器时钟不同步："对端 10:00 发、我 10:03 收"没有意义。
//
// 【E2E 公式】
//   e2e = (包到达本地时刻 − SR到达本地时刻) − RTP时间差(ms) + RTT/2
//
// 【推导】设两端时钟偏移 offset、对称路径：
//   包到达 = 发送时刻(srNtp + rtpDelta) + offset + 单向延迟
//   SR到达 = srNtp + offset + RTT/2
//   两式相减，offset 被消掉（关键！），得到上式。
//   数值自检：RTT=100ms、双向各 50ms，SR t=0 发 t=50 到，
//   包 t=100 发 t=150 到 → (150-50) - 100 + 50 = 50ms ✓
//
// 【RTP 时间差的回绕】uint32 减法转 int32：活跃流的 |差| 恒 < 2^31，
// 补码解释天然正确（与 JitterBuffer 的 16 位 seq 技巧同宗）。
void MetricsCollector::onPacketArrival(uint64_t nowMs, uint32_t rtpTs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 没有 SR 锚点或 RTT 时算不了（开局前几秒的正常状态）
    if (!hasSrAnchor_ || !hasRtt_) return;
    // RTP 时间差转毫秒：clockRate_ 视频 90000 / 音频 48000
    int32_t d = static_cast<int32_t>(rtpTs - srRtpTs_);
    double rtpDeltaMs = static_cast<double>(d) * 1000.0 / clockRate_;
    double e2e = static_cast<double>(nowMs - srArrivalMs_) - rtpDeltaMs
                + rttMs_ / 2.0;
    e2eSamplesMs_.emplace_back(nowMs, e2e);
    evictWindowsLocked(nowMs);
}

// ----------------------------------------------------------------------------
// setRttMs - RTT 喂入（E2E 公式的 RTT/2 项）
// ----------------------------------------------------------------------------
void MetricsCollector::setRttMs(double rttMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    hasRtt_ = true;
    rttMs_ = rttMs;
}

// ----------------------------------------------------------------------------
// onBytesSent - 发送打点：5s 滑动窗口字节累计
// ----------------------------------------------------------------------------
void MetricsCollector::onBytesSent(uint64_t nowMs, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    sentBytes_.emplace_back(nowMs, bytes);
    evictWindowsLocked(nowMs);
}

// ----------------------------------------------------------------------------
// 查询接口
// ----------------------------------------------------------------------------
uint64_t MetricsCollector::stallCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stallCount_;
}

double MetricsCollector::renderFps() const { return renderFpsAt(0); }

// renderFpsAt - 指定时刻的帧率；nowMs==0 表示用最新一帧时刻当窗口右端
double MetricsCollector::renderFpsAt(uint64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (renderTsMs_.empty()) return 0.0;
    if (nowMs == 0) nowMs = renderTsMs_.back();
    evictWindowsLocked(nowMs);
    if (renderTsMs_.empty()) return 0.0;
    return static_cast<double>(renderTsMs_.size()) / 5.0;
}

bool MetricsCollector::hasE2e() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasSrAnchor_ && hasRtt_ && !e2eSamplesMs_.empty();
}

// e2eDelayMs - 最近 5s 窗口内 E2E 的最小值
// 【为什么取 min 而不是均值】排队抖动只会让样本变大不会变小，
// min 滤掉抖动后得到接近"路径本底"的 E2E；均值会被突发抖动拉高。
// （工程上常同时上报 min/p95，本实现保最小闭环，指南里说明差异。）
double MetricsCollector::e2eDelayMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e2eSamplesMs_.empty()) return 0.0;
    double best = e2eSamplesMs_.front().second;
    for (const auto& [ts, v] : e2eSamplesMs_) best = std::min(best, v);
    return best;
}

// sendBitrateKbps - 最近 5s 发送码率：窗口字节 × 8 / 5000ms
// 【窗口右端的取法】用最新打点时刻而非"当前真实时刻"——查询接口不读
// 时钟（可测性原则），且业务上"上次发送以来的码率"本就该从最后一个包
// 算起（发送暂停时窗口内容不凭空衰减，语义更稳）。
double MetricsCollector::sendBitrateKbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sentBytes_.empty()) return 0.0;
    uint64_t nowMs = sentBytes_.back().first;  // 窗口右端 = 最新打点时刻
    evictWindowsLocked(nowMs);
    if (sentBytes_.empty()) return 0.0;
    size_t total = 0;
    for (const auto& [ts, n] : sentBytes_) total += n;
    // kbps = 字节 × 8 bit/字节 / 5000ms（窗口 5s）
    return static_cast<double>(total) * 8.0 / 5000.0;
}

// summaryLine - 拼接可读指标行
// 【格式设计】视频流 = "渲染 X.Xfps | 卡顿 N | E2E Xms | 发送 Xkbps"
//              音频流 = "E2E Xms | 发送 Xkbps"（expectedFps_==0 跳过帧率段）
// 【缺失值显示 "-" 而不是 0】0 会被误读为"延迟为零"，"-" 明确表示
// "尚未就绪"（例如通话刚开始 RTT 还没算出来）——可观测性指标的
// 基本素养：宁缺勿假。
std::string MetricsCollector::summaryLine() const {
    char buf[160];
    std::string head;
    if (expectedFps_ > 0) {
        snprintf(buf, sizeof(buf), "渲染 %.1ffps | 卡顿 %lu | ",
                 renderFps(), static_cast<unsigned long>(stallCount()));
        head = buf;
    }
    std::string e2e = hasE2e() ? std::to_string(
                          static_cast<int>(e2eDelayMs())) + "ms" : "-";
    snprintf(buf, sizeof(buf), "E2E %s | 发送 %.0fkbps",
             e2e.c_str(), sendBitrateKbps());
    return head + buf;
}

// ----------------------------------------------------------------------------
// evictWindowsLocked - 滑动窗口统一清理（5 秒）
// ----------------------------------------------------------------------------
// 【const + mutable 的工程惯用法】查询接口（const）顺手清理窗口，
// 避免为了清理把查询接口被迫改成非 const。窗口类指标的通用手法。
void MetricsCollector::evictWindowsLocked(uint64_t nowMs) const {
    while (!renderTsMs_.empty() && renderTsMs_.front() + 5000 <= nowMs)
        renderTsMs_.pop_front();
    while (!e2eSamplesMs_.empty() && e2eSamplesMs_.front().first + 5000 <= nowMs)
        e2eSamplesMs_.pop_front();
    while (!sentBytes_.empty() && sentBytes_.front().first + 5000 <= nowMs)
        sentBytes_.pop_front();
}

} // namespace crystal
