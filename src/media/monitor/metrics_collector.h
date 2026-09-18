// ============================================================================
// metrics_collector.h - 可观测性指标采集器（工程化升级 v2 新增）
// ============================================================================
//
// 【为什么需要这个类】
//   之前的 [stats] 行只有丢包/抖动/RTT（网络侧），但用户感知到的是
//   "画面卡没卡""声音断没断"（体验侧）。真实工程里体验指标才是北极星：
//   网络指标好不等于体验好（比如码率没变但帧率掉了）。
//   本类把体验侧四个核心指标收口到一处，每条流一个实例。
//
// 【四个指标与算法】
//   1. 卡顿次数：渲染帧间隔 > 1.5×预期帧距（30fps→阈值 50ms）累计。
//      这是行业通用定义（libwebrtc 的 freeze/jank 判定同思路）。
//   2. 实际渲染帧率：5s 滑动窗口内渲染帧数 / 5。
//   3. 端到端延迟：SR 锚点法（见 .cpp 的 onPacketArrival 注释，
//      双端时钟不同步也能算）。
//   4. 发送码率：5s 滑动窗口字节累计 × 8 / 5000（kbps）。
//
// 【线程模型】
//   onRenderedFrame 可能从解码线程调用，onBytesSent 从采集线程调用，
//   查询从主循环调用 —— 三方并发，用一把 mutex 保护。
//   事件率低（每秒几十次），mutex 完全够用；真正的无锁优化在
//   Phase C 的 SPSC 环形缓冲再讲（那里是每秒几千次的热路径）。
//
// 【可测性设计】
//   所有时间戳由调用方传入（uint64_t nowMs），类内部从不读系统时钟。
//   这样单测可以构造任意时间序列（造回绕、造抖动），不依赖真实时间。
//
// ============================================================================

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace crystal {

class MetricsCollector {
public:
    // clockRate   - 该流 RTP 时钟（视频 90000 / 音频 48000），E2E 换算用
    // expectedFps - 预期渲染帧率；0 表示不统计卡顿/帧率（音频流）
    explicit MetricsCollector(uint32_t clockRate, uint32_t expectedFps);

    // ---- 以下为打点接口（时间全部由调用方传入，便于单测造时间序列）----

    // 渲染回调线程：每渲染一帧调用（nowMs 为渲染时刻）
    void onRenderedFrame(uint64_t nowMs);

    // 接收路径：每个 RTP 包到达时调用（E2E 采样）
    void onPacketArrival(uint64_t nowMs, uint32_t rtpTs);

    // 收到对端 SR 时调用（rtpTs 为 SR 携带的 RTP 锚点，srArrivalMs 为 SR 到达时刻）
    void onSenderReportMapping(uint32_t srRtpTs, uint64_t srArrivalMs);

    // 本流 RTT 已测得时喂入（毫秒；未测得前 E2E 无法计算）
    void setRttMs(double rttMs);

    // 发送路径：每发出一包调用（bytes = 含 RTP 头的整包字节数）
    void onBytesSent(uint64_t nowMs, size_t bytes);

    // ---- 以下为查询接口（主循环每 5s 读一次）----

    uint64_t stallCount() const;      // 累计卡顿次数
    double renderFps() const;         // 最近 5s 实际渲染帧率（无渲染则 0）
    double renderFpsAt(uint64_t nowMs) const;  // 指定时刻的帧率（单测用）
    bool hasE2e() const;              // E2E 是否已可算（有 SR 锚点且 RTT 已知）
    double e2eDelayMs() const;        // 最近 5s 窗口内 E2E 最小值（毫秒）
    double sendBitrateKbps() const;   // 最近 5s 发送码率（kbps）

    // 拼接一行可读指标（音频流自动省略帧率/卡顿段）
    std::string summaryLine() const;

private:
    // 统一在锁内清理过期窗口样本（nowMs 为当前时刻）
    void evictWindowsLocked(uint64_t nowMs) const;

    uint32_t clockRate_;
    uint32_t expectedFps_;
    mutable std::mutex mutex_;

    // --- 卡顿/帧率 ---
    bool hasLastRender_ = false;
    uint64_t lastRenderMs_ = 0;
    uint64_t stallCount_ = 0;
    // 三个窗口队列声明 mutable：const 查询接口里顺手清理窗口（见 .cpp 注释）
    mutable std::deque<uint64_t> renderTsMs_;   // 5s 滑动窗口

    // --- E2E ---
    bool hasSrAnchor_ = false;
    uint32_t srRtpTs_ = 0;
    uint64_t srArrivalMs_ = 0;
    bool hasRtt_ = false;
    double rttMs_ = 0;
    mutable std::deque<std::pair<uint64_t, double>> e2eSamplesMs_;  // (到达时刻, E2E样本)

    // --- 发送码率 ---
    mutable std::deque<std::pair<uint64_t, size_t>> sentBytes_;  // (时刻, 字节)
};

} // namespace crystal
