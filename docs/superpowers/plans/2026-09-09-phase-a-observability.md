# Phase A：可观测性指标 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 MetricsCollector，为每条流统计卡顿次数、实际渲染帧率、端到端延迟、发送码率，并接入 main_client 每 5s 输出 `[metrics]` 行。

**Architecture:** 每条流（视频/音频）一个 `MetricsCollector` 实例，多线程安全（std::mutex，事件率低无需无锁）。E2E 延迟用"SR 到达锚点 + RTP 时间差 + RTT/2"公式，双端时钟不同步也成立。新增静态库 `crystal_media_monitor`，只依赖 crystal_utils。

**Tech Stack:** C++17、googletest、spdlog（仅 main_client 打日志，collector 本身不依赖日志）。

**设计文档:** `docs/superpowers/specs/2026-09-09-crystalrtc-v2-engineering-upgrade-design.md` 第 3 节

**E2E 延迟公式（本计划核心算法，Task 3 实现）：**

```
e2e = (包到达本地时刻 − 最近一次SR到达本地时刻) − RTP时间差(转ms) + RTT/2
```

推导：SR 携带对端 NTP↔RTP 锚点。包的发送时刻（对端时钟）= srNtp + rtpDelta。设两端时钟偏移 offset，则 包到达 = srNtp + rtpDelta + offset + 单向延迟，SR 到达 = srNtp + offset + RTT/2（对称路径假设）。两式相减消去 offset 得上式。数值自检：RTT=100ms、双向各 50ms，SR 发于 t=0 到达 50，包发于 t=100 到达 150 → (150−50)−100+50 = 50ms ✓。取 5s 窗口内**最小值**（min 滤掉排队抖动，得到接近路径本底的 E2E）。

---

### Task 1: MetricsCollector 骨架 + 卡顿计数 + 渲染帧率

**Files:**
- Create: `src/media/monitor/metrics_collector.h`
- Create: `src/media/monitor/metrics_collector.cpp`
- Test: `tests/metrics_collector_test.cpp`

- [ ] **Step 1: 写失败测试（卡顿 + 帧率）**

创建 `tests/metrics_collector_test.cpp`：

```cpp
// ============================================================================
// metrics_collector_test.cpp - 可观测性指标采集器单测
// ============================================================================
#include "media/monitor/metrics_collector.h"
#include <gtest/gtest.h>

// 卡顿判定：渲染帧间隔 > 1.5×预期帧距（30fps → 33.3ms 预期，50ms 阈值）
TEST(MetricsCollector, StallCounting) {
    crystal::MetricsCollector m(90000, 30);
    m.onRenderedFrame(0);      // 第一帧只建立基准
    m.onRenderedFrame(33);     // 正常间隔
    m.onRenderedFrame(66);     // 正常间隔
    m.onRenderedFrame(150);    // 距上帧 84ms > 50ms → 卡顿 1 次
    m.onRenderedFrame(183);    // 正常间隔
    m.onRenderedFrame(400);    // 距上帧 217ms → 卡顿 2 次
    EXPECT_EQ(m.stallCount(), 2u);
}

// 帧率：5s 滑动窗口内渲染帧数 / 5
TEST(MetricsCollector, RenderFpsWindow) {
    crystal::MetricsCollector m(90000, 30);
    for (int i = 0; i < 100; ++i) m.onRenderedFrame(i * 33);  // 前 3.3s，100 帧
    EXPECT_NEAR(m.renderFpsAt(4000), 100 / 5.0, 0.01);         // 窗口 [0,5000) 全含
    EXPECT_NEAR(m.renderFpsAt(3400), 100 / 5.0, 0.01);         // 窗口仍全含
    // 窗口推进到 [3500,8500)：只剩 3500 之后的帧（i*33>=3500 → i>=107 无，但旧帧已逐出）
    // 33*106=3498 <3500 已逐出，实际剩 0 帧
    EXPECT_NEAR(m.renderFpsAt(9000), 0.0, 0.01);
}

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /workspace/build && cmake -S .. -B . -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build . --target crystal_metrics_tests -j4 2>&1 | head -5`
Expected: FAIL —— `metrics_collector.h: No such file or directory`（文件尚未创建，且 CMake 目标尚不存在时先报 CMake 错误，属预期失败）

- [ ] **Step 3: 创建 CMake 目标与实现**

`src/CMakeLists.txt` 末尾追加：

```cmake
# --- Media: Monitor（工程化升级 v2：可观测性）---
add_library(crystal_media_monitor STATIC
    media/monitor/metrics_collector.cpp
)
target_include_directories(crystal_media_monitor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_monitor PUBLIC crystal_utils)
```

`tests/CMakeLists.txt` 末尾追加：

```cmake
add_executable(crystal_metrics_tests
    metrics_collector_test.cpp
)
target_link_libraries(crystal_metrics_tests PRIVATE
    crystal_media_monitor crystal_utils GTest::gtest_main
)
add_test(NAME crystal_metrics_tests COMMAND crystal_metrics_tests)
```

创建 `src/media/monitor/metrics_collector.h`：

```cpp
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
//   3. 端到端延迟：见 Task 3 的公式注释（SR 锚点法，时钟无关）。
//   4. 发送码率：5s 滑动窗口字节累计 × 8 / 5000（kbps）。
//
// 【线程模型】
//   onRenderedFrame 可能从解码线程调用，onBytesSent 从采集线程调用，
//   查询从主循环调用 —— 三方并发，用一把 mutex 保护。
//   事件率低（每秒几十次），mutex 完全够用；真正的无锁优化在
//   Phase C 的 SPSC 环形缓冲再讲（那里是每秒几千次的热路径）。
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
    std::deque<uint64_t> renderTsMs_;   // 5s 滑动窗口

    // --- E2E ---
    bool hasSrAnchor_ = false;
    uint32_t srRtpTs_ = 0;
    uint64_t srArrivalMs_ = 0;
    bool hasRtt_ = false;
    double rttMs_ = 0;
    std::deque<double> e2eSamplesMs_;   // 5s 滑动窗口内每包一个样本

    // --- 发送码率 ---
    std::deque<std::pair<uint64_t, size_t>> sentBytes_;  // (时刻, 字节)
};

} // namespace crystal
```

创建 `src/media/monitor/metrics_collector.cpp`（Task 1 只实现卡顿/帧率相关，其余方法留空壳在 Task 2/3 补全——空壳直接返回默认值，不是 TODO）：

```cpp
// ============================================================================
// metrics_collector.cpp - 可观测性指标采集器实现（工程化升级 v2 新增）
// ============================================================================

#include "media/monitor/metrics_collector.h"
#include <algorithm>

namespace crystal {

MetricsCollector::MetricsCollector(uint32_t clockRate, uint32_t expectedFps)
    : clockRate_(clockRate), expectedFps_(expectedFps) {}

void MetricsCollector::onRenderedFrame(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // --- 卡顿检测：帧间隔超过 1.5 倍预期帧距即计一次 ---
    // 30fps 预期帧距 33.3ms，阈值 50ms；阈值放宽 1.5 倍是为了
    // 不把偶发的调度毛刺（一次 GC/页缺失）算成卡顿，只抓真掉帧。
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

void MetricsCollector::onPacketArrival(uint64_t /*nowMs*/, uint32_t /*rtpTs*/) {
    // Task 3 实现（E2E 采样）
}

void MetricsCollector::onSenderReportMapping(uint32_t /*srRtpTs*/,
                                             uint64_t /*srArrivalMs*/) {
    // Task 3 实现（SR 锚点记录）
}

void MetricsCollector::setRttMs(double rttMs) {
    // Task 3 接入（E2E 需要 RTT/2）
    std::lock_guard<std::mutex> lock(mutex_);
    hasRtt_ = true;
    rttMs_ = rttMs;
}

void MetricsCollector::onBytesSent(uint64_t /*nowMs*/, size_t /*bytes*/) {
    // Task 2 实现（发送码率窗口）
}

uint64_t MetricsCollector::stallCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stallCount_;
}

double MetricsCollector::renderFps() const { return renderFpsAt(0); }

double MetricsCollector::renderFpsAt(uint64_t nowMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    // nowMs==0 表示"用最新一帧时刻当窗口右端"（主循环查询用）
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

void MetricsCollector::evictWindowsLocked(uint64_t nowMs) const {
    // 滑动窗口统一 5 秒：把所有窗口里 5s 前的样本逐出。
    // const 成员里改 deque 需 mutable（查询时顺手清理，工程惯用手法，
    // 避免查询接口被迫变成非 const）
    while (!renderTsMs_.empty() && renderTsMs_.front() + 5000 <= nowMs)
        renderTsMs_.pop_front();
    while (!e2eSamplesMs_.empty()) e2eSamplesMs_.pop_front();  // Task 3 带时间戳化
    while (!sentBytes_.empty() && sentBytes_.front().first + 5000 <= nowMs)
        sentBytes_.pop_front();
}

} // namespace crystal
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /workspace/build && cmake -S .. -B . -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests --gtest_filter='MetricsCollector.*'`
Expected: `[==========] 2 tests from 1 test suite ran` → `2 PASSED`（StallCounting、RenderFpsWindow）

- [ ] **Step 5: 提交**

```bash
git add src/media/monitor/ tests/metrics_collector_test.cpp src/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(monitor): MetricsCollector 骨架——卡顿计数与渲染帧率滑动窗口"
```

---

### Task 2: 发送码率滑动窗口

**Files:**
- Modify: `src/media/monitor/metrics_collector.cpp`（onBytesSent / sendBitrateKbps）
- Test: `tests/metrics_collector_test.cpp`（追加）

- [ ] **Step 1: 写失败测试**

`tests/metrics_collector_test.cpp` 追加：

```cpp
// 发送码率：5s 窗口内字节 × 8 / 5000ms
TEST(MetricsCollector, SendBitrateWindow) {
    crystal::MetricsCollector m(90000, 30);
    // t=0 发 5000 字节，t=1000 发 5000 字节
    m.onBytesSent(0, 5000);
    m.onBytesSent(1000, 5000);
    // 全窗口 10000 字节 → 10000×8/5000ms = 16kbps
    EXPECT_NEAR(m.sendBitrateKbps(), 16.0, 0.01);

    // t=5100 时 t=0 的样本已逐出，只剩 t=1000 的 5000 字节 → 8kbps
    // （sendBitrateKbps 内部用最新样本时刻当窗口右端）
    m.onBytesSent(5100, 0);   // 0 字节打点，仅推进窗口右端
    EXPECT_NEAR(m.sendBitrateKbps(), 8.0, 0.01);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /workspace/build && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests --gtest_filter='MetricsCollector.SendBitrateWindow'`
Expected: FAIL —— 期望 16.0，实际 0.0

- [ ] **Step 3: 实现 onBytesSent / sendBitrateKbps**

`metrics_collector.cpp` 中替换空壳：

```cpp
void MetricsCollector::onBytesSent(uint64_t nowMs, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    sentBytes_.emplace_back(nowMs, bytes);
    evictWindowsLocked(nowMs);
}
```

```cpp
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
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /workspace/build && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests --gtest_filter='MetricsCollector.SendBitrateWindow'`
Expected: PASSED

- [ ] **Step 5: 提交**

```bash
git add src/media/monitor/metrics_collector.cpp tests/metrics_collector_test.cpp
git commit -m "feat(monitor): 发送码率 5s 滑动窗口统计"
```

---

### Task 3: E2E 端到端延迟（SR 锚点法）

**Files:**
- Modify: `src/media/monitor/metrics_collector.cpp`（onSenderReportMapping / onPacketArrival / e2eDelayMs，e2e 窗口改带时间戳）
- Modify: `src/media/monitor/metrics_collector.h`（e2eSamplesMs_ 改为 pair 队列）
- Test: `tests/metrics_collector_test.cpp`（追加）

- [ ] **Step 1: 写失败测试**

`tests/metrics_collector_test.cpp` 追加：

```cpp
// E2E：SR 锚点 + RTP 时间差 + RTT/2，取 5s 窗口最小值
TEST(MetricsCollector, E2eDelay) {
    crystal::MetricsCollector m(90000, 30);   // 视频时钟 90kHz
    // 对端 SR 在 t=0ms 到达本地，携带 RTP 锚点 ts=0
    m.onSenderReportMapping(0, 0);
    m.setRttMs(100.0);                        // RTT 100ms → RTT/2 = 50ms

    // 包 A：RTP ts = 9000（= 发送时刻 100ms 后），t=150ms 到达本地
    //   实际单向延迟 = 150 - 100 = 50ms；公式 = (150-0) - 100 + 50 = 100ms？
    //   注意公式含 RTT/2 是为消时钟偏移；本测试两端时钟对齐（offset=0），
    //   对称路径下 SR 到达也应为 50ms 而非 0 —— 造数据时 SR 到达时刻应为 50：
    m.onSenderReportMapping(0, 50);           // 修正：SR t=50 到达（发送侧 t=0 发出）
    m.onPacketArrival(150, 9000);             // 包在发送侧 t=100 发出，t=150 到达
    //   e2e = (150 - 50) - 100 + 50 = 50ms ✓
    ASSERT_TRUE(m.hasE2e());
    EXPECT_NEAR(m.e2eDelayMs(), 50.0, 0.01);

    // 包 B：网络抖动导致 80ms 才到（排队 30ms）
    m.onPacketArrival(230, 18000);            // 发送侧 t=200，到达 t=230
    EXPECT_NEAR(m.e2eDelayMs(), 50.0, 0.01);  // min 仍是 50（B 是 80）

    // RTP 时间戳回绕：锚点 ts=65535 附近
    crystal::MetricsCollector m2(48000, 0);
    m2.onSenderReportMapping(65535, 1000);
    m2.setRttMs(80.0);                        // RTT/2 = 40ms
    // 音频 48kHz：锚点后 480 采样 = 10ms；包到达应比 SR 晚 10+40+e2e
    m2.onPacketArrival(1050, 15);             // 15 = 65535+480 回绕
    //   e2e = (1050-1000) - 10 + 40 = 80ms
    ASSERT_TRUE(m2.hasE2e());
    EXPECT_NEAR(m2.e2eDelayMs(), 80.0, 0.01);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /workspace/build && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests --gtest_filter='MetricsCollector.E2eDelay'`
Expected: FAIL —— hasE2e() 为 false（onPacketArrival 是空壳）

- [ ] **Step 3: 实现 E2E 采样**

`metrics_collector.h` 中把成员改带时间戳（供窗口清理用）：

```cpp
    // 旧：std::deque<double> e2eSamplesMs_;
    std::deque<std::pair<uint64_t, double>> e2eSamplesMs_;  // (到达时刻, E2E样本)
```

`metrics_collector.cpp` 中替换空壳（evictWindowsLocked 里对应的 e2e 逐出行同步改为 pair 版本）：

```cpp
void MetricsCollector::onSenderReportMapping(uint32_t srRtpTs,
                                             uint64_t srArrivalMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    hasSrAnchor_ = true;
    srRtpTs_ = srRtpTs;
    srArrivalMs_ = srArrivalMs;
}
```

```cpp
void MetricsCollector::onPacketArrival(uint64_t nowMs, uint32_t rtpTs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 没有 SR 锚点或 RTT 时算不了（开局前几秒的正常状态）
    if (!hasSrAnchor_ || !hasRtt_) return;
    // RTP 时间差：int32_t 转换天然处理 16 位……这里是 32 位时间戳回绕，
    // 活跃流的 |差| 恒小于 2^31，补码解释正确（同 JitterBuffer seq 技巧）
    int32_t d = static_cast<int32_t>(rtpTs - srRtpTs_);
    double rtpDeltaMs = static_cast<double>(d) * 1000.0 / clockRate_;
    // 【E2E 公式】(到达 − SR到达) − RTP时间差 + RTT/2
    //   推导见计划头部注释：RTT/2 项用于对消两端时钟偏移，
    //   对称路径假设下结果 = 包的真实单向网络延迟。
    double e2e = static_cast<double>(nowMs - srArrivalMs_) - rtpDeltaMs
                + rttMs_ / 2.0;
    e2eSamplesMs_.emplace_back(nowMs, e2e);
    evictWindowsLocked(nowMs);
}
```

```cpp
double MetricsCollector::e2eDelayMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (e2eSamplesMs_.empty()) return 0.0;
    // 取窗口最小值：排队抖动被滤掉，得到接近路径本底的 E2E。
    // （工程上也会同时上报 p95，这里取 min 保最小实现，指南里说明差异）
    double best = e2eSamplesMs_.front().second;
    for (const auto& [ts, v] : e2eSamplesMs_) best = std::min(best, v);
    return best;
}
```

`evictWindowsLocked` 中 e2e 逐出行改为：

```cpp
    while (!e2eSamplesMs_.empty() && e2eSamplesMs_.front().first + 5000 <= nowMs)
        e2eSamplesMs_.pop_front();
```

- [ ] **Step 4: 跑全量测试确认通过**

Run: `cd /workspace/build && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests`
Expected: `4 PASSED`（StallCounting、RenderFpsWindow、SendBitrateWindow、E2eDelay）

- [ ] **Step 5: 提交**

```bash
git add src/media/monitor/metrics_collector.cpp src/media/monitor/metrics_collector.h tests/metrics_collector_test.cpp
git commit -m "feat(monitor): E2E 延迟——SR 锚点法消时钟偏移，5s 窗口取最小值"
```

---

### Task 4: summaryLine 拼接 + 全库接入构建

**Files:**
- Modify: `src/media/monitor/metrics_collector.cpp`（summaryLine）
- Test: `tests/metrics_collector_test.cpp`（追加）

- [ ] **Step 1: 写失败测试**

`tests/metrics_collector_test.cpp` 追加：

```cpp
TEST(MetricsCollector, SummaryLine) {
    crystal::MetricsCollector video(90000, 30);
    // 喂一帧 + 若干字节，格式含关键字段即可（不测精确数字，测结构）
    video.onRenderedFrame(0);
    video.onBytesSent(0, 1000);
    std::string s = video.summaryLine();
    EXPECT_NE(s.find("fps"), std::string::npos);
    EXPECT_NE(s.find("卡顿"), std::string::npos);

    crystal::MetricsCollector audio(48000, 0);
    std::string a = audio.summaryLine();
    // 音频流不该出现帧率段
    EXPECT_EQ(a.find("fps"), std::string::npos);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cd /workspace/build && cmake --build . --target crystal_metrics_tests -j4 && ./tests/crystal_metrics_tests --gtest_filter='MetricsCollector.SummaryLine'`
Expected: FAIL —— summaryLine 返回 ""，找不到 "fps"

- [ ] **Step 3: 实现 summaryLine**

`metrics_collector.cpp` 替换空壳：

```cpp
std::string MetricsCollector::summaryLine() const {
    // 拼接规则：视频流 = 渲染 fps | 卡顿 | E2E | 发送码率
    //           音频流 = E2E | 发送码率（expectedFps_==0 跳过帧率段）
    // 数值缺失（如 E2E 未就绪）显示 "-"，不显示 0 以免误读
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
```

注意：头文件需补 `#include <cstdio>`。

- [ ] **Step 4: 全目标构建 + 全量测试**

Run: `cd /workspace/build && cmake --build . -j4 && ctest --output-on-failure`
Expected: crystal_rtp_tests / crystal_rtcp_tests / crystal_metrics_tests 全部 PASSED

- [ ] **Step 5: 提交**

```bash
git add src/media/monitor/metrics_collector.cpp src/media/monitor/metrics_collector.h tests/metrics_collector_test.cpp
git commit -m "feat(monitor): summaryLine 指标行拼接，音频流自动省略帧率段"
```

---

### Task 5: main_client.cpp 接线

**Files:**
- Modify: `main_client.cpp`（四处接线 + [metrics] 输出行）

- [ ] **Step 1: 头部 include 与实例创建**

`main_client.cpp` 的 include 区（现有 `media/rtp/...` 系列之后）加：

```cpp
#include "media/monitor/metrics_collector.h"  // 工程化升级 v2：可观测性
```

在 `crystal::RtpDepacketizer depacketizer;`（约 L165）之后加实例：

```cpp
    // ---- 可观测性指标（工程化升级 v2 新增）----
    // 每条流一个采集器：视频统计帧率/卡顿/E2E/码率，音频只统计 E2E/码率
    crystal::MetricsCollector videoMetrics(90000, encConfig.fps);
    crystal::MetricsCollector audioMetrics(48000, 0);
```

- [ ] **Step 2: 发送路径打点**

视频发送回调 `encoder.onEncoded` 里，`retxBuffer.store(...)` 一行之后、`videoSendReport.onPacketSent(...)` 之前插：

```cpp
            videoMetrics.onBytesSent(nowMs(), data.size());  // 体验指标：码率
```

音频发送回调 `alsaCapture.onAudio` 的 for 循环内，`audioSendReport.onPacketSent(...)` 之后插：

```cpp
            audioMetrics.onBytesSent(nowMs(), data.size());  // 体验指标：码率
```

- [ ] **Step 3: 渲染路径打点（卡顿/帧率）**

`decoder.onDecoded` 回调（渲染处）改为：

```cpp
    decoder.onDecoded([&](const uint8_t* yuvData, int width, int height) {
        renderer.render(yuvData, width, height);
        videoMetrics.onRenderedFrame(nowMs());  // 工程化升级 v2：卡顿/帧率打点
    });
```

- [ ] **Step 4: 接收路径打点（E2E）与 SR 锚点**

`pc->onTrack` 回调里，`if (pkt.payloadType() == 96) {` 分支的统计调用后插：

```cpp
            videoMetrics.onPacketArrival(nowMs(), pkt.timestamp());  // E2E 采样
```

`else if (pkt.payloadType() == 97)` 分支同理：

```cpp
            audioMetrics.onPacketArrival(nowMs(), pkt.timestamp());  // E2E 采样
```

`pc->onRtcp` 的 `case crystal::RtcpKind::SenderReport:` 分支改为（按 SSRC 分发锚点）：

```cpp
                case crystal::RtcpKind::SenderReport: {
                    // 对端 SR → 记录到达时刻（我方 RR 报告块 LSR/DLSR 基准）
                    uint64_t ntp = crystal::nowNtp();
                    videoRecvReport.onSenderReport(p.sr.ssrc, ntp);
                    audioRecvReport.onSenderReport(p.sr.ssrc, ntp);
                    // 工程化升级 v2：SR 同时携带 NTP↔RTP 锚点，
                    // 喂给对应流的 MetricsCollector 供 E2E 计算
                    if (p.sr.ssrc == videoRecvReport.remoteSsrc()) {
                        videoMetrics.onSenderReportMapping(p.sr.rtpTimestamp,
                                                           nowMs());
                    } else if (p.sr.ssrc == audioRecvReport.remoteSsrc()) {
                        audioMetrics.onSenderReportMapping(p.sr.rtpTimestamp,
                                                           nowMs());
                    }
                    break;
                }
```

- [ ] **Step 5: 主循环输出 [metrics] 行 + RTT 喂入**

主循环 5s 统计块里，现有 `[stats]` 日志之后追加：

```cpp
            // 工程化升级 v2：体验侧指标（网络好≠体验好，这是北极星）
            videoMetrics.setRttMs(videoSendReport.hasRtt()
                                      ? videoSendReport.rttMs() : 0);
            if (videoSendReport.hasRtt())
                videoMetrics.setRttMs(videoSendReport.rttMs());
            audioMetrics.setRttMs(audioSendReport.hasRtt()
                                      ? audioSendReport.rttMs() : 0);
            if (audioSendReport.hasRtt())
                audioMetrics.setRttMs(audioSendReport.rttMs());
            crystal::Logger::info("[metrics][v] {}", videoMetrics.summaryLine());
            crystal::Logger::info("[metrics][a] {}", audioMetrics.summaryLine());
```

（注：先无条件喂 0 再在 hasRtt 时喂真值——setRttMs(0) 会让 hasRtt_=true 但值为 0，导致 E2E 样本按 RTT=0 计算。**正确做法是只喂真值**，删掉三元写法，最终代码为：）

```cpp
            // 工程化升级 v2：体验侧指标（网络好≠体验好，这是北极星）
            if (videoSendReport.hasRtt())
                videoMetrics.setRttMs(videoSendReport.rttMs());
            if (audioSendReport.hasRtt())
                audioMetrics.setRttMs(audioSendReport.rttMs());
            crystal::Logger::info("[metrics][v] {}", videoMetrics.summaryLine());
            crystal::Logger::info("[metrics][a] {}", audioMetrics.summaryLine());
```

`main_client.cpp` 的链接：根 `CMakeLists.txt` 的 `crystal_client` 目标需链接 `crystal_media_monitor`（若根文件按库枚举链接，把 `crystal_media_monitor` 加进 target_link_libraries 列表）。

- [ ] **Step 6: 全量构建验证**

Run: `cd /workspace/build && cmake --build . -j4 && ctest --output-on-failure`
Expected: 全部测试 PASSED，crystal_client 链接成功

- [ ] **Step 7: 提交**

```bash
git add main_client.cpp CMakeLists.txt
git commit -m "feat(monitor): main_client 接入 MetricsCollector，每 5s 输出 [metrics] 体验指标行"
```

---

### Task 6: LEARNING_GUIDE.md 更新（可观测性章节）

**Files:**
- Modify: `docs/LEARNING_GUIDE.md`（Module 10 之后、面试专题之前插入新模块；目录同步）

- [ ] **Step 1: 目录区（## 目录 的列表）加一行**

```markdown
- [Module 13：可观测性指标【工程化升级 v2 新增】](#module-13可观测性指标工程化升级-v2-新增)
```

- [ ] **Step 2: 在 `## 面试专题` 标题之前插入完整模块**

````markdown
## Module 13：可观测性指标【工程化升级 v2 新增】

> 本模块为工程化升级 v2 新增内容。之前所有模块解决"功能能不能跑"，
> 本模块解决"体验好不好"——这是工作中的北极星指标。

### 概念讲解

#### 为什么网络指标好 ≠ 体验好

丢包率 0%、RTT 30ms 的链路上，如果编码器因为某个 bug 每秒只出 10 帧，
用户照样觉得卡。`[stats]` 行（网络侧）与 `[metrics]` 行（体验侧）的分工：

| 指标 | 类型 | 回答的问题 |
|------|------|------------|
| 丢包/抖动/RTT | 网络侧 | 链路质量如何 |
| 渲染帧率/卡顿次数 | 体验侧 | 画面流畅吗 |
| E2E 延迟 | 体验侧 | 从采集到看到隔多久 |
| 发送码率 | 资源侧 | 占了多少带宽 |

#### 卡顿的判定标准

帧间隔 > 1.5×预期帧距（30fps → 50ms）计一次。为什么是 1.5 倍而不是
2 倍/精确值？单帧调度毛刺（一次缺页、一次日志刷盘）不该算卡顿，
连续性掉帧才该算。libwebrtc 的 jank/freeze 判定同思路。

#### E2E 延迟怎么算：SR 锚点法（本模块最难也最有面试价值）

问题：两台机器时钟不同步，"对端 10:00 发的包我 10:03 收到"没有意义。
解决：利用 SR 里 NTP↔RTP 时间戳对（同一个时刻的两种表示）+ RTT：

```
e2e = (包到达本地时刻 − SR到达本地时刻) − RTP时间差(ms) + RTT/2
```

推导：设两端时钟偏移 offset、对称路径。SR 在对端时钟 srNtp 时刻发出，
本地 A_sr 时刻到达；包在 srNtp+rtpDelta 发出、A_p 到达。两式相减
消掉 offset（推导细节见 metrics_collector.cpp 头注释）。RTT/2 来自
Phase 1 的 LSR/DLSR 法——注意那是"我们→对端"方向的 RTT，对称路径
假设下复用。最后取 5s 窗口最小值：网络排队抖动被滤掉，得到接近
路径本底的 E2E。

【生产差异】libwebrtc 用 absolute capture time / TWCC 时间戳做更精确的
分段延迟（采集→编码→打包→网络→缓冲→解码→渲染），本实现是网络段
E2E 的最小闭环。真实工作中你会看到 WebRTC-Internals 里那条
"Total delay (ms)" 曲线。

### 代码精读

文件：`src/media/monitor/metrics_collector.h/.cpp`

重点读三处：
1. `onRenderedFrame` 的卡顿判定（1.5 倍阈值的三行代码）
2. `onPacketArrival` 的 E2E 公式（对照上面的推导）
3. `evictWindowsLocked` 的滑动窗口清理（const + mutable 的工程惯用法：
   查询接口顺手清理窗口，避免查询被迫非 const）

### 动手练习

**练习 13.1（改阈值）：** 把卡顿阈值从 1.5 倍改成 2 倍，构造练习 13.2 的
帧序列，对比卡顿计数差异，体会阈值敏感性。

**练习 13.2（造数据）：** 写个小 main，手动调 onRenderedFrame(0/33/66/150/400)，
验证卡顿=2（与单测同数据）。

### 面试高频题

1. **怎么测 WebRTC 的端到端延迟？** SR 锚点法：NTP↔RTP 映射 + RTT/2
   消时钟偏移；更精确用 TWCC/absolute capture time 分段测量。
2. **卡顿怎么定义？** 帧间隔超阈值（我们 1.5×帧距）；追问会引申到
   freeze（>500ms 停顿）与 jank（单帧超时）的行业区分。
3. **为什么指标要分网络侧/体验侧？** 北极星是体验；网络指标是归因手段。
   举例：卡顿升高 + 丢包不变 → 查编码/渲染线程，而不是查网络。
4. **滑动窗口为什么 5s？** 与 SR/RR 周期对齐（RFC 3550 推荐 5s），
   太短毛刺多，太长反应慢。工程上指标窗口常取 1s/5s/60s 三档。
````

- [ ] **Step 3: 面试专题追问清单追加**

在 `#### 工程化（4 条）` 小节之后追加一节：

```markdown
#### 可观测性（4 条）【工程化升级 v2 新增】

1. 端到端延迟怎么测？时钟不同步怎么办？（SR 锚点法 + RTT/2）
2. 卡顿的定义和阈值怎么定？freeze 和 jank 的区别？
3. 网络指标和体验指标为什么要分开看？举个"网络好但体验差"的例子。
4. 音频卡顿和视频卡顿的检测有什么不同？（下溢计数 vs 帧间隔；
   音频缓冲水位 Phase C 实现）
```

- [ ] **Step 4: 提交**

```bash
git add docs/LEARNING_GUIDE.md
git commit -m "docs: 指南新增 Module 13 可观测性（工程化升级 v2 标记）"
```

---

## Self-Review 记录

1. **Spec 覆盖**：设计文档第 3 节的 5 项指标中，"音频缓冲水位/下溢"依赖 Phase C 环形缓冲，本计划不含（设计文档已注明"Phase C 完成后接入"）✓ 其余 4 项（卡顿、帧率、E2E、码率）均有任务 ✓
2. **占位符扫描**：Task 1 的空壳方法在 Task 2/3 替换为完整实现，无 TBD/TODO ✓
3. **类型一致性**：`renderFpsAt`（测试用）/`renderFps`（查询用）双接口在头文件与测试中名称一致；e2eSamplesMs_ 在 Task 3 从 `deque<double>` 改 `deque<pair>` 并同步改 evict 行 ✓
4. **Task 5 Step 5** 中先展示了错误写法（无条件 setRttMs(0)）再给正确写法——保留这个"错误示范"是刻意的教学材料（指南风格），接线时**只用最终代码**。
