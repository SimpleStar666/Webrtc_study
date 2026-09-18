// ============================================================================
// metrics_collector.cpp - 可观测性指标采集器实现（工程化升级 v2 新增）
// ============================================================================

#include "media/monitor/metrics_collector.h"
#include <algorithm>

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
// onPacketArrival - E2E 采样（Task 3 实现）
// ----------------------------------------------------------------------------
void MetricsCollector::onPacketArrival(uint64_t /*nowMs*/, uint32_t /*rtpTs*/) {
}

// ----------------------------------------------------------------------------
// onSenderReportMapping - SR 锚点记录（Task 3 实现）
// ----------------------------------------------------------------------------
void MetricsCollector::onSenderReportMapping(uint32_t /*srRtpTs*/,
                                             uint64_t /*srArrivalMs*/) {
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
// onBytesSent - 发送码率窗口（Task 2 实现）
// ----------------------------------------------------------------------------
void MetricsCollector::onBytesSent(uint64_t /*nowMs*/, size_t /*bytes*/) {
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

double MetricsCollector::e2eDelayMs() const { return 0.0; }  // Task 3 实现

double MetricsCollector::sendBitrateKbps() const { return 0.0; }  // Task 2 实现

std::string MetricsCollector::summaryLine() const {
    // Task 4 实现（拼 [metrics] 行）
    return "";
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
