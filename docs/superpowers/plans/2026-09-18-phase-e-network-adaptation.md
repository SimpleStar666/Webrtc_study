# Phase E：网络自适应（AdaptationController）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 GCC 之上建跨流网络自适应层：网络分级 → 帧率钳制 / FEC 上限 / PLI 决策，含连接状态监控、Demo 6 与文档。

**Architecture:** 新库 `crystal_media_adaptive`（NetworkQualityClassifier 纯函数分级 + AdaptationController 去抖决策 + FramerateThrottler 编码线程执行器）。主循环 200ms tick 收集信号（GCC 目标码率/RTT/丢包率），决策经原子交接给编码线程与 Opus FEC 通道；ICE 底层不动，只轮询 `pc->state()` 观测。

**Tech Stack:** C++17、googletest、现有 crystal_* 库体系（见 `src/CMakeLists.txt`）。

**设计文档:** `docs/superpowers/specs/2026-09-18-phase-e-network-adaptation-design.md`

**关键既有接口（勿改签名）:**
- `crystal::GccController::targetBitrateKbps()` — [gcc_controller.h:52](file:///workspace/src/media/gcc/gcc_controller.h#L52)
- `crystal::SendSideReporter::hasRtt()/rttMs()/remoteFractionLost()` — [rtcp_reporter.h:51-53](file:///workspace/src/media/rtcp/rtcp_reporter.h#L51-L53)（fraction lost 8 位定点，255=100%）
- `videoRecvReport.jitterMs()`、`videoMetrics.stallCount()` — RecvSideReporter/MetricsCollector
- `sendPli()` lambda（main_client.cpp:282，自带 500ms 节流与 SSRC 检查）— Bad 级直接复用
- `rtc::PeerConnection::State`：New/Connecting/Connected/Disconnected/Failed/Closed（libdatachannel v0.21.2，build/_deps 已核实）
- `nowMs()` 文件级工具（main_client.cpp:113）；编码线程帧节奏见 main_client.cpp:648-684

---

### Task 1: NetworkQualityClassifier + 新库/测试目标 CMake

**Files:**
- Create: `src/media/adaptive/network_quality_classifier.h`
- Create: `src/media/adaptive/network_quality_classifier.cpp`
- Modify: `src/CMakeLists.txt`（追加 crystal_media_adaptive 库，加在 crystal_media_gcc 之后）
- Modify: `tests/CMakeLists.txt`（追加 crystal_adaptive_tests 目标）
- Test: `tests/network_classifier_test.cpp`

- [ ] **Step 1: 建 CMake 骨架 + 占位文件（测试先要有目标可跑）**

`src/CMakeLists.txt` 末尾（crystal_media_gcc 块之后）追加：

```cmake
# --- Media: Adaptive（工程化升级 v2/Phase E：网络自适应）---
add_library(crystal_media_adaptive STATIC
    media/adaptive/network_quality_classifier.cpp
    media/adaptive/adaptation_controller.cpp
)
target_include_directories(crystal_media_adaptive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_adaptive PUBLIC crystal_utils)
```

`tests/CMakeLists.txt` 末尾追加：

```cmake
# 网络自适应测试（工程化升级 v2 Phase E 新增）
add_executable(crystal_adaptive_tests
    network_classifier_test.cpp
    framerate_throttler_test.cpp
)
target_link_libraries(crystal_adaptive_tests PRIVATE
    crystal_media_adaptive crystal_utils GTest::gtest_main
)
add_test(NAME crystal_adaptive_tests COMMAND crystal_adaptive_tests)
```

同 Step 创建三个占位文件（Task 2/3 替换为完整实现）：

`src/media/adaptive/adaptation_controller.h`：

```cpp
#pragma once
#include "media/adaptive/network_quality_classifier.h"
#include <cstdint>
// Task 2 完整实现
namespace crystal {
}
```

`src/media/adaptive/adaptation_controller.cpp`：

```cpp
#include "media/adaptive/adaptation_controller.h"
// Task 2 实现
```

`tests/framerate_throttler_test.cpp`：

```cpp
// Task 3 实现
#include <gtest/gtest.h>
```

- [ ] **Step 2: 写失败测试（分类器）**

`tests/network_classifier_test.cpp`：

```cpp
// ============================================================================
// network_classifier_test.cpp — 网络分级器 + 自适应控制器测试（Phase E）
// ============================================================================
// 分级器是纯函数（无时钟无锁），直接构造 NetworkSignals 断言等级。
// 控制器测试注入时间序列（nowMs 由调用方传入），验证去抖/阶梯/PLI。
// ============================================================================
#include <gtest/gtest.h>
#include "media/adaptive/network_quality_classifier.h"

using crystal::NetworkQuality;
using crystal::NetworkSignals;

static NetworkSignals makeSignals(uint32_t gccKbps, uint32_t configuredKbps,
                                  double lossPct, double rttMs) {
    NetworkSignals s;
    s.gccTargetKbps = gccKbps;
    s.configuredKbps = configuredKbps;
    s.lossPct = lossPct;
    s.rttMs = rttMs;
    return s;
}

TEST(NetworkClassifierTest, ClassifiesGoodAtFullBandwidth) {
    auto s = makeSignals(1000, 1000, 0.5, 40);   // ratio=1.0
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Good);
}

TEST(NetworkClassifierTest, ClassifiesFairAtModerateDegradation) {
    auto s = makeSignals(700, 1000, 3.0, 200);  // ratio=0.7, rtt<300
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Fair);
}

TEST(NetworkClassifierTest, ClassifiesPoorMiddleGround) {
    auto s = makeSignals(500, 1000, 8.0, 250);  // 未达 Fair 也未恶劣到 Bad
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Poor);
}

TEST(NetworkClassifierTest, ClassifiesBadOnRatioCollapse) {
    auto s = makeSignals(200, 1000, 1.0, 100);  // ratio=0.2 < 0.35
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Bad);
}

TEST(NetworkClassifierTest, ClassifiesBadOnHighLoss) {
    auto s = makeSignals(700, 1000, 20.0, 100); // loss >= 15%
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Bad);
}

TEST(NetworkClassifierTest, ClassifiesBadOnHighRtt) {
    auto s = makeSignals(700, 1000, 1.0, 900);  // rtt > 800ms
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Bad);
}

TEST(NetworkClassifierTest, UnknownRttDoesNotBlockGood) {
    auto s = makeSignals(950, 1000, 1.0, -1.0); // rtt 未知：不参与判级
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Good);
}

TEST(NetworkClassifierTest, UnknownRttStillBadOnRatio) {
    auto s = makeSignals(200, 1000, 1.0, -1.0); // rtt 未知，但 ratio 命中 Bad
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Bad);
}

TEST(NetworkClassifierTest, LossBeatsPoorOnCascade) {
    // 级联顺序验证：ratio=0.5 落在 Poor 区间，但 loss=20 命中 Bad → Bad
    auto s = makeSignals(500, 1000, 20.0, 250);
    EXPECT_EQ(crystal::classifyNetwork(s), NetworkQuality::Bad);
}

TEST(NetworkClassifierTest, QualityNameRoundTrip) {
    EXPECT_STREQ(crystal::networkQualityName(NetworkQuality::Good), "Good");
    EXPECT_STREQ(crystal::networkQualityName(NetworkQuality::Bad), "Bad");
}
```

- [ ] **Step 3: 跑测试确认编译失败**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests 2>&1 | tail -5`
Expected: FAIL（`classifyNetwork` 未定义——头文件还没写）

- [ ] **Step 4: 实现分类器**

`src/media/adaptive/network_quality_classifier.h`：

```cpp
// ============================================================================
// network_quality_classifier.h - 网络分级器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// GCC 回答"带宽是多少"，本组件回答"网络处于什么状态"——后者是跨流
// 策略（降帧率/FEC 上限/PLI）的输入。对齐 libwebrtc 里 Call 级
// NetworkController 的信号汇聚角色（简化为单函数分级）。
//
// 【纯函数设计（可测性）】
// 无时钟、无锁、无副作用：输入信号结构体，输出等级。所有阈值是
// 常量，单测直接构造边界值验证。真实工程的阈值随场景调参
// （会议/直播/1v1 各不同），本实现取 WebRTC 通话的典型量级。
//
// 【为什么用级联而不是独立区间】
// 阈值区间重叠时（如 ratio=0.5 且 loss=20% 同时落 Poor 与 Bad 的
// 字面条件），级联判定（Good→Fair→Bad，命中即返回，兜底 Poor）
// 保证结果唯一——这是状态机防二义性的标准写法。
//
// 【信号说明】
// - ratio = gccTargetKbps / configuredKbps：可用带宽相对编码器期望的
//   比例。GCC 收敛后 ratio<1 说明带宽撑不起当前配置，是降级的核心理由
// - rttMs < 0 表示未知（SR/RR 还没配对出 RTT）：该条件视为不命中，
//   不阻塞判级也不误触发 Bad
// - jitterMs/freezeCount 本阶段仅随 [net] 行观测输出，不参与判级
//   （链路信号与体验信号混判容易互斥打架，见指南"生产差异"）
// ============================================================================
#pragma once

#include <cstdint>

namespace crystal {

// 网络质量等级（数值序即劣化序：Good < Fair < Poor < Bad）
enum class NetworkQuality { Good, Fair, Poor, Bad };

// 分级输入信号（主循环每 200ms 收集一次）
struct NetworkSignals {
    uint32_t gccTargetKbps = 1000;  // GCC 当前目标码率
    uint32_t configuredKbps = 1000; // 编码器初始配置码率（ratio 分母）
    double rttMs = -1.0;            // SR/RR 配对 RTT；<0 未知
    double lossPct = 0.0;          // 对端 RR 观测丢包率（百分比）
    double jitterMs = 0.0;         // 平滑抖动（仅观测）
    uint32_t freezeCount = 0;      // 渲染卡顿累计（仅观测）
};

// 级联判定：Good → Fair → Bad（命中即返回），兜底 Poor
NetworkQuality classifyNetwork(const NetworkSignals& s);

// 等级名（日志/[net] 行用）
const char* networkQualityName(NetworkQuality q);

} // namespace crystal
```

`src/media/adaptive/network_quality_classifier.cpp`：

```cpp
#include "media/adaptive/network_quality_classifier.h"

namespace crystal {

NetworkQuality classifyNetwork(const NetworkSignals& s) {
    // 带宽压力比：GCC 能给的 / 编码器想要的（分母为 0 视为完全受限）
    double ratio = s.configuredKbps > 0
                       ? static_cast<double>(s.gccTargetKbps) / s.configuredKbps
                       : 0.0;

    // rtt 未知（<0）：两个 rtt 条件都视为不命中（不阻塞也不触发）
    bool rttOkGood = (s.rttMs < 0) || (s.rttMs < 150.0);
    bool rttOkFair = (s.rttMs < 0) || (s.rttMs < 300.0);

    if (ratio >= 0.85 && s.lossPct < 2.0 && rttOkGood)
        return NetworkQuality::Good;
    if (ratio >= 0.60 && s.lossPct < 5.0 && rttOkFair)
        return NetworkQuality::Fair;
    if (ratio < 0.35 || s.lossPct >= 15.0 || s.rttMs > 800.0)
        return NetworkQuality::Bad;
    return NetworkQuality::Poor;
}

const char* networkQualityName(NetworkQuality q) {
    switch (q) {
        case NetworkQuality::Good: return "Good";
        case NetworkQuality::Fair:  return "Fair";
        case NetworkQuality::Poor:  return "Poor";
        case NetworkQuality::Bad:   return "Bad";
    }
    return "Unknown";
}

} // namespace crystal
```

同 Step 为 Task 2/3 占位的最小骨架：

`src/media/adaptive/adaptation_controller.cpp`：

```cpp
#include "media/adaptive/adaptation_controller.h"
// Task 2 实现
```

`src/media/adaptive/adaptation_controller.h`（最小占位，Task 2 完整化）：

```cpp
#pragma once
#include "media/adaptive/network_quality_classifier.h"
#include <cstdint>
// Task 2 完整实现
namespace crystal {
}
```

`tests/framerate_throttler_test.cpp`（占位）：

```cpp
// Task 3 实现
#include <gtest/gtest.h>
```

- [ ] **Step 5: 构建并跑测试**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests && ./build/tests/crystal_adaptive_tests --gtest_filter='NetworkClassifierTest.*'`
Expected: 全部 PASS（10 个用例）

- [ ] **Step 6: 提交**

```bash
git add src/media/adaptive/ src/CMakeLists.txt tests/CMakeLists.txt tests/network_classifier_test.cpp tests/framerate_throttler_test.cpp
git commit -m "feat(adaptive): 网络分级器 NetworkQualityClassifier + 新库骨架"
```

---

### Task 2: AdaptationController（去抖状态机 + 帧率阶梯 + 决策表）

**Files:**
- Modify: `src/media/adaptive/adaptation_controller.h`（替换占位）
- Modify: `src/media/adaptive/adaptation_controller.cpp`（替换占位）
- Test: `tests/network_classifier_test.cpp`（追加控制器用例）

- [ ] **Step 1: 追加失败测试**

`tests/network_classifier_test.cpp` 末尾追加（头部补 `#include "media/adaptive/adaptation_controller.h"`）：

```cpp
// ---------------------------------------------------------------------------
// AdaptationController：去抖 / 帧率阶梯 / FEC 上限 / PLI edge
// 时间序列注入：tick(signals, nowMs)，nowMs 每 200ms 递增
// ---------------------------------------------------------------------------
using crystal::AdaptationController;

// 喂 N 个同等级信号的便捷函数（nowMs 从 start 每 200ms 递增）
static crystal::AdaptationDecision feedTicks(AdaptationController& c,
                                             const NetworkSignals& s,
                                             int n, uint64_t startMs) {
    crystal::AdaptationDecision d;
    for (int i = 0; i < n; ++i) d = c.tick(s, startMs + i * 200);
    return d;
}

static NetworkSignals badSignals()   { return makeSignals(200, 1000, 20.0, 900); }
static NetworkSignals poorSignals() { return makeSignals(500, 1000, 8.0, 250); }
static NetworkSignals goodSignals()  { return makeSignals(1000, 1000, 0.5, 40); }

TEST(AdaptationControllerTest, DowngradeNeedsThreeConsecutiveTicks) {
    AdaptationController c(1000, 30);
    auto d = feedTicks(c, badSignals(), 2, 0);       // 连续 2 tick Bad
    EXPECT_EQ(d.level, NetworkQuality::Good);        // 不足 3：不降
    d = c.tick(badSignals(), 600);                   // 第 3 tick
    EXPECT_EQ(d.level, NetworkQuality::Bad);         // 快降生效
}

TEST(AdaptationControllerTest, UpgradeNeedsTenConsecutiveTicks) {
    AdaptationController c(1000, 30);
    feedTicks(c, badSignals(), 3, 0);                // 先降到 Bad
    auto d = feedTicks(c, goodSignals(), 9, 1000);   // 连续 9 tick Good
    EXPECT_EQ(d.level, NetworkQuality::Bad);        // 不足 10：不升
    d = c.tick(goodSignals(), 2800);                 // 第 10 tick
    EXPECT_EQ(d.level, NetworkQuality::Good);        // 慢升生效
}

TEST(AdaptationControllerTest, PingPongSignalsKeepLevel) {
    AdaptationController c(1000, 30);
    // Good/Bad 交替：candidate 永远凑不满 3，等级保持
    for (int i = 0; i < 20; ++i) {
        auto d = (i % 2 == 0) ? c.tick(goodSignals(), i * 200)
                              : c.tick(badSignals(), i * 200);
        EXPECT_EQ(d.level, NetworkQuality::Good);
    }
}

TEST(AdaptationControllerTest, PoorDropsFpsToHalfRung) {
    AdaptationController c(1000, 30);
    auto d = feedTicks(c, poorSignals(), 3, 0);      // 降到 Poor
    EXPECT_EQ(d.level, NetworkQuality::Poor);
    EXPECT_EQ(d.targetFps, 15u);                    // 阶梯 30→15（减半档）
}

TEST(AdaptationControllerTest, BadDropsFpsToFloor) {
    AdaptationController c(1000, 30);
    auto d = feedTicks(c, badSignals(), 3, 0);      // 降到 Bad
    EXPECT_EQ(d.targetFps, 10u);                    // 下限 10
}

TEST(AdaptationControllerTest, FecCapPerLevel) {
    AdaptationController c(1000, 30);
    auto d = feedTicks(c, goodSignals(), 1, 0);
    EXPECT_EQ(d.fecCapPct, 40u);                    // Good/Fair：40%
    d = feedTicks(c, poorSignals(), 3, 400);
    EXPECT_EQ(d.fecCapPct, 50u);                    // Poor：放宽到 50%
    d = feedTicks(c, badSignals(), 3, 1000);
    EXPECT_EQ(d.fecCapPct, 30u);                    // Bad：收紧到 30%
}

TEST(AdaptationControllerTest, PliFiresOncePerBadEntry) {
    AdaptationController c(1000, 30);
    auto d = feedTicks(c, badSignals(), 3, 0);      // 进入 Bad
    EXPECT_TRUE(d.requestPli);                      // 进入瞬间请求一次
    d = c.tick(badSignals(), 600);                   // 维持 Bad
    EXPECT_FALSE(d.requestPli);                     // 不重复请求（edge）
}

TEST(AdaptationControllerTest, LadderClimbsWithMinDwell) {
    AdaptationController c(1000, 30);
    feedTicks(c, badSignals(), 3, 0);               // Bad → fps=10
    // 恢复 Good：等级 10 tick 后切换，帧率每 2s 升一级
    std::vector<uint32_t> fpsHistory;
    for (int i = 0; i < 40; ++i) {                  // 8s
        auto d = c.tick(goodSignals(), 1000 + i * 200);
        if (fpsHistory.empty() || fpsHistory.back() != d.targetFps)
            fpsHistory.push_back(d.targetFps);
    }
    // 10 → 12 → 15 → 20 → 30（每级至少停 2s，不许一步跳回）
    EXPECT_EQ(fpsHistory.back(), 30u);
    for (uint32_t expect : {12u, 15u, 20u})
        EXPECT_NE(std::find(fpsHistory.begin(), fpsHistory.end(), expect),
                  fpsHistory.end());
    EXPECT_EQ(fpsHistory.size(), 5u);               // 10,12,15,20,30
}
```

文件头部追加 `#include <algorithm>` 与 `#include <vector>`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests 2>&1 | tail -5`
Expected: FAIL（`AdaptationDecision`/`AdaptationController` 未定义）

- [ ] **Step 3: 实现控制器**

`src/media/adaptive/adaptation_controller.h` 全量替换为：

```cpp
// ============================================================================
// adaptation_controller.h - 网络自适应控制器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
// 【职责与分层（对齐 libwebrtc Call 级）】
//   GCC       ：带宽估计（targetKbps 是多少）—— 不动，本层只消费
//   本控制器  ：跨流策略（带宽不够时帧率降多少、FEC 上限收到哪、
//               要不要 PLI）—— 网络状态 → 统一决策
//   执行器    ：FramerateThrottler（编码线程）/ opusLossPct_ 通道
//               （音频采集线程）—— 原子交接，各自应用
//
// 【防乒乓：快降慢升】
//   降级 3 tick（600ms）生效，升级 10 tick（2s）生效。
//   降级快：网络恶化每 600ms 都在持续伤体验，晚降级 = 持续卡顿
//   升级慢：带宽恢复初期不稳（WiFi 漫游回切/拥塞缓释），立刻升回
//           容易再次打爆带宽来回震荡。不对称是 AIMD 同款哲学。
//
// 【帧率阶梯】
//   {100%, 2/3, 50%, 40%, 33%} × 配置帧率，下限 10fps：
//   降级直接跳档（Poor→50% 档，Bad→下限），升级每级至少停 2s
//   （minDwell）逐级爬。降帧率不重配编码器（x264 上下文不动），
//   对齐 libwebrtc MaintainResolution 的 frame dropping 路径。
//
//【FEC 上限曲线（protection overhead）】
//   Good/Fair 40% / Poor 50% / Bad 30%：
//   Poor 放宽——丢包率真实升高，需要更多冗余；
//   Bad 收紧——链路极度受限时，过度 FEC 会挤占视频有效载荷，
//   保护性开销的带宽分配是真实工程的经典权衡。
//
// 【PLI edge 语义】
//   进入 Bad 瞬间 requestPli=true（一次），消费即清。Bad 期间不重复
//   请求（发送侧 sendPli 已有 500ms 节流，双保险防关键帧风暴）。
//
//【线程模型】
//   仅主循环 200ms tick 调用，无锁；决策经 atomic/值拷贝交接执行器。
// ============================================================================
#pragma once

#include "media/adaptive/network_quality_classifier.h"
#include <cstdint>
#include <vector>

namespace crystal {

// 一次 tick 的决策输出（值语义返回，调用方自行交接）
struct AdaptationDecision {
    NetworkQuality level = NetworkQuality::Good;
    uint32_t targetFps = 30;     // 编码线程目标帧率
    uint32_t fecCapPct = 40;     // Opus FEC 冗余度上限（%）
    bool requestPli = false;     // edge：本次 tick 进入 Bad 时为 true
    uint64_t levelAgeMs = 0;     // 当前等级已持续时间（观测用）
};

class AdaptationController {
public:
    // configuredKbps - 编码器初始码率（ratio 分母）
    // configuredFps  - 编码器配置帧率（阶梯基準）
    AdaptationController(uint32_t configuredKbps, uint32_t configuredFps);

    // 主循环每 200ms 驱动：分级 → 去抖 → 阶梯 → 组装决策
    AdaptationDecision tick(const NetworkSignals& signals, uint64_t nowMs);

    // 最近一次决策（[net] 行随 5s 周期读取）
    const AdaptationDecision& lastDecision() const { return decision_; }

    static constexpr uint32_t kMinFps = 10;        // 帧率下限
    static constexpr int kDowngradeTicks = 3;      // 降级去抖（600ms）
    static constexpr int kUpgradeTicks = 10;       // 升级去抖（2s）
    static constexpr uint64_t kMinDwellMs = 2000;  // 帧率档位最小停留

private:
    // 帧率阶梯：降级跳档 / 升级逐级（受 minDwell 约束）
    void applyLadder(NetworkQuality level, uint64_t nowMs);
    size_t currentRung() const;
    static std::vector<uint32_t> buildLadder(uint32_t configuredFps);
    static uint32_t fecCapFor(NetworkQuality q);

    uint32_t configuredKbps_;
    uint32_t configuredFps_;
    std::vector<uint32_t> ladder_;      // 降序：ladder_[0]=配置帧率 … back()=下限

    // --- 去抖状态机 ---
    NetworkQuality level_ = NetworkQuality::Good;     // 当前生效等级
    NetworkQuality candidate_ = NetworkQuality::Good; // 正在累计的候选
    int candidateStreak_ = 0;                        // 候选连续次数
    uint64_t levelSinceMs_ = 0;                       // 本级生效起始

    // --- 阶梯状态 ---
    uint32_t targetFps_;
    uint64_t lastLadderMoveMs_ = 0;                   // 上次移档时刻

    bool pliPending_ = false;                          // 进入 Bad 置位
    AdaptationDecision decision_;
};

} // namespace crystal
```

`src/media/adaptive/adaptation_controller.cpp` 全量替换为：

```cpp
#include "media/adaptive/adaptation_controller.h"
#include <algorithm>

namespace crystal {

AdaptationController::AdaptationController(uint32_t configuredKbps,
                                           uint32_t configuredFps)
    : configuredKbps_(configuredKbps),
      configuredFps_(configuredFps),
      ladder_(buildLadder(configuredFps)),
      targetFps_(configuredFps) {
    decision_.targetFps = configuredFps;
    decision_.level = NetworkQuality::Good;
}

std::vector<uint32_t> AdaptationController::buildLadder(uint32_t fps) {
    // 阶梯比例 {100%, 2/3, 50%, 40%, 33%}，逐级钳到下限并去重
    // 30fps → {30, 20, 15, 12, 10}（设计文档同款）
    const double kFractions[] = {1.0, 2.0 / 3.0, 0.5, 0.4, 1.0 / 3.0};
    std::vector<uint32_t> ladder;
    for (double f : kFractions) {
        uint32_t v = std::max(kMinFps,
                              static_cast<uint32_t>(fps * f + 0.5));
        if (ladder.empty() || v < ladder.back())
            ladder.push_back(v);   // 严格降序（低帧率配置会自然缩短阶梯）
    }
    return ladder;
}

uint32_t AdaptationController::fecCapFor(NetworkQuality q) {
    switch (q) {
        case NetworkQuality::Poor: return 50;  // 丢包真实升高：放宽
        case NetworkQuality::Bad:  return 30;  // 链路受限：防 FEC 挤占载荷
        default:                   return 40;  // Good/Fair
    }
}

size_t AdaptationController::currentRung() const {
    for (size_t i = 0; i < ladder_.size(); ++i)
        if (ladder_[i] == targetFps_) return i;
    return 0;  // 不在阶梯上：按最高档处理（下一轮升降会归位）
}

void AdaptationController::applyLadder(NetworkQuality level, uint64_t nowMs) {
    size_t idx = currentRung();

    if (level == NetworkQuality::Poor || level == NetworkQuality::Bad) {
        // 降级跳档：Poor → 减半档；Bad → 下限（实时视频宁可降质不可断流）
        uint32_t want = (level == NetworkQuality::Bad)
                            ? ladder_.back()
                            : std::max(kMinFps, configuredFps_ / 2);
        for (size_t i = 0; i < ladder_.size(); ++i) {
            if (ladder_[i] <= want && ladder_[i] < targetFps_) {
                targetFps_ = ladder_[i];
                lastLadderMoveMs_ = nowMs;
                return;
            }
        }
        return;  // 已在目标档或更低：不动（防重复移档刷新 dwell 计时）
    }

    // 升级：Good/Fair 逐级爬，每级至少停留 kMinDwellMs
    if (idx > 0 && nowMs - lastLadderMoveMs_ >= kMinDwellMs) {
        targetFps_ = ladder_[idx - 1];
        lastLadderMoveMs_ = nowMs;
    }
}

AdaptationDecision AdaptationController::tick(const NetworkSignals& s,
                                              uint64_t nowMs) {
    NetworkQuality raw = classifyNetwork(s);

    // --- 去抖状态机：快降慢升 ---
    if (raw != level_) {
        if (raw == candidate_) {
            ++candidateStreak_;
        } else {
            candidate_ = raw;
            candidateStreak_ = 1;
        }
        // 枚举数值序 = 劣化序（Good=0 … Bad=3）
        bool worse = static_cast<int>(raw) > static_cast<int>(level_);
        int needed = worse ? kDowngradeTicks : kUpgradeTicks;
        if (candidateStreak_ >= needed) {
            level_ = candidate_;
            candidateStreak_ = 0;
            levelSinceMs_ = nowMs;
            if (level_ == NetworkQuality::Bad)
                pliPending_ = true;   // 进入 Bad：请求关键帧恢复画面
        }
    } else {
        candidate_ = level_;
        candidateStreak_ = 0;         // 回到当前级：计数清零（防乒乓核心）
    }

    // --- 帧率阶梯 ---
    applyLadder(level_, nowMs);

    // --- 组装决策 ---
    decision_.level = level_;
    decision_.targetFps = targetFps_;
    decision_.fecCapPct = fecCapFor(level_);
    decision_.levelAgeMs = nowMs - levelSinceMs_;
    decision_.requestPli = pliPending_;
    pliPending_ = false;              // edge：暴露一次即消费
    return decision_;
}

} // namespace crystal
```

- [ ] **Step 4: 构建并跑测试**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests && ./build/tests/crystal_adaptive_tests`
Expected: 全部 PASS（分类器 10 + 控制器 8）

注意 `LadderClimbsWithMinDwell` 的时序推演：Bad 移档发生在 tick#2（nowMs=400），等级 10 tick 后（nowMs=2800）切 Good；dwell = 2800-400 = 2400 ≥ 2000 → 立刻升 12；之后 2000ms 一级 → 15/20/30，40 tick（8s）内完成，fpsHistory = {10,12,15,20,30}。

- [ ] **Step 5: 提交**

```bash
git add src/media/adaptive/adaptation_controller.h src/media/adaptive/adaptation_controller.cpp tests/network_classifier_test.cpp
git commit -m "feat(adaptive): AdaptationController 去抖状态机+帧率阶梯+FEC上限"
```

---

### Task 3: FramerateThrottler（编码线程策略性丢帧）

**Files:**
- Create: `src/media/adaptive/framerate_throttler.h`（header-only）
- Test: `tests/framerate_throttler_test.cpp`（替换占位）

- [ ] **Step 1: 写失败测试**

`tests/framerate_throttler_test.cpp` 全量替换为：

```cpp
// ============================================================================
// framerate_throttler_test.cpp — 帧率节流器测试（Phase E）
// ============================================================================
// 注入时间序列验证丢帧节奏：30fps 源降到 15/10fps 时，该编的编、
// 该丢的丢，恢复后回满帧率。policyDropped 与 backlog 丢帧正交。
// ============================================================================
#include <gtest/gtest.h>
#include "media/adaptive/framerate_throttler.h"

TEST(FramerateThrottlerTest, FullRateWhenUnthrottled) {
    crystal::FramerateThrottler t(30);
    // 30fps 源：33ms 帧距全部放行（间隔 1000/30=33，33 不小于 33）
    EXPECT_TRUE(t.shouldEncode(0));
    EXPECT_TRUE(t.shouldEncode(33));
    EXPECT_TRUE(t.shouldEncode(66));
    EXPECT_EQ(t.policyDroppedCount(), 0u);
}

TEST(FramerateThrottlerTest, HalvesRateAt15Fps) {
    crystal::FramerateThrottler t(30);
    t.setTargetFps(15);   // 主循环降档发布
    // 30fps 源（33ms 帧距）：编 0，丢 33，编 66，丢 99，编 132
    EXPECT_TRUE(t.shouldEncode(0));
    EXPECT_FALSE(t.shouldEncode(33));   // 33 < 66：策略丢
    EXPECT_TRUE(t.shouldEncode(66));    // 66 >= 66：放行
    EXPECT_FALSE(t.shouldEncode(99));
    EXPECT_TRUE(t.shouldEncode(132));
    EXPECT_EQ(t.policyDroppedCount(), 2u);
}

TEST(FramerateThrottlerTest, RecoversWhenFpsRestored) {
    crystal::FramerateThrottler t(30);
    t.setTargetFps(10);
    EXPECT_TRUE(t.shouldEncode(0));
    EXPECT_FALSE(t.shouldEncode(33));
    t.setTargetFps(30);   // 恢复满帧率
    EXPECT_TRUE(t.shouldEncode(66));
    EXPECT_TRUE(t.shouldEncode(99));    // 33 间隔重新全部放行
    EXPECT_TRUE(t.shouldEncode(132));
}

TEST(FramerateThrottlerTest, ZeroFpsMeansNoThrottle) {
    crystal::FramerateThrottler t(30);
    t.setTargetFps(0);    // 防御：0 视为不节流
    EXPECT_TRUE(t.shouldEncode(0));
    EXPECT_TRUE(t.shouldEncode(1));
}
```

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests 2>&1 | tail -5`
Expected: FAIL（`FramerateThrottler` 未定义）

- [ ] **Step 3: 实现节流器**

`src/media/adaptive/framerate_throttler.h`：

```cpp
// ============================================================================
// framerate_throttler.h - 帧率节流器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
//【在自适应链路中的角色】
// AdaptationController 决策 targetFps 后，由本执行器在编码线程落地：
// 距上一帧不足 1000/targetFps 的采集帧按策略丢弃。
//
//【与 Phase D backlog 丢帧的正交性（面试考点）】
//   backlog 丢帧：SpscRing 积压满（编码跟不上采集）——被动、编码器问题
//   policy 丢帧 ：带宽不够主动降帧率（本类）——主动、网络策略
//   两者都丢帧但含义完全不同，[net] 行分别计数。
//
//【为什么不重配编码器（degradation preference）】
// libwebrtc 降帧率走编码前丢帧（VideoStreamEncoder 的 frame dropper），
// 不动编码器上下文：重配 x264（分辨率/帧率参数）需要重建上下文，
// 重配瞬间可能卡顿。分辨率降级才走 ReconfigureEncoder 重路径。
//
//【线程模型】
// setTargetFps 主循环 store / shouldEncode 编码线程 load（原子交接）；
// lastEncodedMs_/policyDropped_ 仅编码线程访问，无需同步。
//
//【整数除法说明】
// 1000/fps 向下取整（30→33ms），30fps 源按 33ms 帧距刚好全部放行，
// 不会误丢；10fps 时源帧距 33ms 的 3 倍是 99<100，实际 ~7.6fps
// 轻微欠调——教学实现可接受，精确版可用微秒时钟。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace crystal {

class FramerateThrottler {
public:
    explicit FramerateThrottler(uint32_t initialFps)
        : targetFps_(initialFps) {}

    // 主循环：Adaptation 决策发布新目标帧率
    void setTargetFps(uint32_t fps) {
        targetFps_.store(fps, std::memory_order_relaxed);
    }
    uint32_t targetFps() const {
        return targetFps_.load(std::memory_order_relaxed);
    }

    // 编码线程：每帧编码前调用；false = 本帧按策略丢弃
    bool shouldEncode(uint64_t nowMs) {
        uint32_t fps = targetFps_.load(std::memory_order_relaxed);
        if (fps == 0) return true;               // 防御：0 = 不节流
        uint64_t minIntervalMs = 1000 / fps;
        if (hasLast_ && nowMs - lastEncodedMs_ < minIntervalMs) {
            ++policyDropped_;
            return false;
        }
        lastEncodedMs_ = nowMs;
        hasLast_ = true;
        return true;
    }

    // 观测：策略丢帧累计（区别于 frameQueue 的 backlog 丢帧）
    uint64_t policyDroppedCount() const { return policyDropped_; }

private:
    std::atomic<uint32_t> targetFps_;
    bool hasLast_ = false;          // 仅编码线程访问
    uint64_t lastEncodedMs_ = 0;     // 仅编码线程访问
    uint64_t policyDropped_ = 0;    // 仅编码线程访问
};

} // namespace crystal
```

- [ ] **Step 4: 构建并跑测试**

Run: `cmake --build build -j"$(nproc)" --target crystal_adaptive_tests && ./build/tests/crystal_adaptive_tests --gtest_filter='FramerateThrottlerTest.*'`
Expected: 4 个用例 PASS

- [ ] **Step 5: 提交**

```bash
git add src/media/adaptive/framerate_throttler.h tests/framerate_throttler_test.cpp
git commit -m "feat(adaptive): FramerateThrottler 编码线程策略性丢帧"
```

---

### Task 4: main_client 集成（200ms tick + 编码线程节流 + [net] 行 + 断线告警）

**Files:**
- Modify: `main_client.cpp`（头部 include 区 / 组件构造区 ~L220 / 编码线程 ~L680 / 主循环 ~L732 / 5s 统计块 ~L814）
- Modify: `CMakeLists.txt`（根，crystal_client 链接新库）

- [ ] **Step 1: 根 CMake 链接**

`CMakeLists.txt`（根）crystal_client 的 whole-archive 组（L57-60）内追加 `crystal_media_adaptive`：

```cmake
target_link_libraries(crystal_client PRIVATE
    -Wl,--whole-archive
    crystal_signaling crystal_transport crystal_media_video crystal_media_audio
    crystal_media_rtp crystal_media_monitor crystal_room crystal_utils
    crystal_media_gcc crystal_media_adaptive
    -Wl,--no-whole-archive
    datachannel websockets spdlog::spdlog nlohmann_json::nlohmann_json
    ${SDL2_LIBRARIES}
    avcodec swresample swscale avutil
    opus asound
)
```

（即在 `crystal_media_gcc` 后加 `crystal_media_adaptive`）

- [ ] **Step 2: main_client 头部与组件构造**

include 区（`#include "media/media/gcc/gcc_controller.h"` 等附近，L71-97 区域内）追加：

```cpp
#include "media/adaptive/adaptation_controller.h"
#include "media/adaptive/framerate_throttler.h"
```

组件构造区（`GccController gcc(...)` 之后、`frameQueue` 之前，约 L226）追加：

```cpp
    // ---- 网络自适应（工程化升级 v2/Phase E 新增）----
    // GCC 之上的跨流策略层：ratio = GCC 目标码率/编码器配置码率，
    // 联合 RTT/丢包率分级 → 帧率钳制 / FEC 上限 / PLI 决策。
    // 无锁：仅主循环 200ms tick 访问，决策经原子交接给执行器
    crystal::AdaptationController adaptation(
        static_cast<uint32_t>(encConfig.bitrateKbps), encConfig.fps);
    // 帧率节流执行器：主循环 store 决策帧率，编码线程 shouldEncode 应用
    crystal::FramerateThrottler fpsThrottler(encConfig.fps);
    // FEC 冗余度上限（Adaptation 决策，主循环应用时与实测取 min）
    uint32_t fecCapPct = 40;
```

- [ ] **Step 3: 编码线程插入节流（pop 之后、encode 之前）**

编码线程（L648-684）中，`videoRtpTs += (1 + skipped) * (90000 / encConfig.fps);` 之后、`encoder.encode(...)` 之前插入：

```cpp
            // --- 策略性丢帧（Phase E）：带宽不足时主动降帧率 ---
            // 与上面的 backlog 丢帧正交：那是"编码跟不上"（被动），
            // 这是"网络不够发"（主动）。不重配编码器——x264 上下文
            // 不动，码率由 GCC 通道单独钳制（MaintainResolution 路径）
            if (!fpsThrottler.shouldEncode(nowMs())) {
                // 策略跳过的帧仍按真实节奏推进 RTP 时钟：
                // 丢帧表现为画面跳一格，而非时间戳漂移后加速播放
                videoRtpTs += 90000 / encConfig.fps;
                continue;
            }
```

- [ ] **Step 4: 主循环 200ms 自适应 tick**

主循环 GCC 100ms 块（L720-732）之后插入：

```cpp
        // --- 每 200ms：网络自适应 tick（Phase E：分级→跨流决策）---
        if (now - lastAdaptMs >= 200) {
            lastAdaptMs = now;
            // 信号收集：GCC 目标码率（压力比分子）+ RTCP 实测
            crystal::NetworkSignals sig;
            sig.gccTargetKbps = gcc.targetBitrateKbps();
            sig.configuredKbps = static_cast<uint32_t>(encConfig.bitrateKbps);
            sig.rttMs = videoSendReport.hasRtt()
                            ? videoSendReport.rttMs() : -1.0;
            // 对端 RR 观测的我方视频流丢包率（8bit 定点 → 百分比）
            sig.lossPct = videoSendReport.remoteFractionLost() * 100.0 / 255.0;
            sig.jitterMs = videoRecvReport.jitterMs();            // 仅观测
            sig.freezeCount =
                static_cast<uint32_t>(videoMetrics.stallCount()); // 仅观测

            auto d = adaptation.tick(sig, now);
            // 决策交接：帧率 → 编码线程（原子），FEC 上限 → 5s 块应用
            fpsThrottler.setTargetFps(d.targetFps);
            fecCapPct = d.fecCapPct;
            // Bad 级进入：请求对端关键帧（复用 sendPli 的 500ms 节流）
            if (d.requestPli) sendPli();

            // --- 连接状态监控（Phase E：ICE 底层归 libdatachannel，
            //     这里只做轮询观测；断线不重连，见指南"生产差异"）---
            lastPcState = pc->state();
            bool linkDown =
                (lastPcState == rtc::PeerConnection::State::Disconnected ||
                 lastPcState == rtc::PeerConnection::State::Failed);
            if (linkDown && !wasLinkDown) {   // 状态沿告警（只报一次）
                crystal::Logger::warn(
                    "[net] 连接状态异常: {}（不做重连，见指南生产差异）",
                    pcStateName(lastPcState));
            }
            wasLinkDown = linkDown;
        }
```

主循环变量声明区（`uint64_t lastGccMs = 0;` 旁，L695 附近）追加：

```cpp
    uint64_t lastAdaptMs = 0;   // 网络自适应 tick 节拍（Phase E，200ms）
    rtc::PeerConnection::State lastPcState = rtc::PeerConnection::State::New;
    bool wasLinkDown = false;
```

文件头部（nowMs 附近，L113 前）追加状态名工具：

```cpp
// 连接状态名（[net] 行/告警用；libdatachannel State 枚举 → 可读名）
static const char* pcStateName(rtc::PeerConnection::State st) {
    switch (st) {
        case rtc::PeerConnection::State::New:         return "new";
        case rtc::PeerConnection::State::Connecting:   return "connecting";
        case rtc::PeerConnection::State::Connected:    return "connected";
        case rtc::PeerConnection::State::Disconnected: return "disconnected";
        case rtc::PeerConnection::State::Failed:       return "failed";
        case rtc::PeerConnection::State::Closed:       return "closed";
    }
    return "unknown";
}
```

- [ ] **Step 5: FEC 上限应用（改既有 opusLossPct_ 写入点）**

5s 统计块中（L814-816）原代码：

```cpp
            opusLossPct_.store(
                audioSendReport.remoteFractionLost() * 100 / 255,
                std::memory_order_relaxed);
```

替换为：

```cpp
            // 实测丢包率与 Adaptation 上限取 min（Phase E）：
            // Poor 放宽（丢包真实升高）、Bad 收紧（过度 FEC 挤占视频
            // 有效载荷——protection overhead 的带宽分配权衡）
            uint32_t measured =
                audioSendReport.remoteFractionLost() * 100 / 255;
            opusLossPct_.store(std::min(measured, fecCapPct),
                               std::memory_order_relaxed);
```

include 区确认有 `#include <algorithm>`（没有则补）。

- [ ] **Step 6: [net] 指标行（5s 块内、[metrics][a] 行之后）**

L805（`audioPlayer.droppedMs()` 日志行）之后插入：

```cpp
            // --- 连接状态 + 自适应决策行（Phase E 新增）---
            crystal::Logger::info(
                "[net] state={} level={} rtt={:.0f}ms | fps策略 {}/{} "
                "fecCap {}% | 策略丢帧 {} 队列丢帧 {}",
                pcStateName(lastPcState),
                crystal::networkQualityName(adaptation.lastDecision().level),
                videoSendReport.hasRtt() ? videoSendReport.rttMs() : 0.0,
                adaptation.lastDecision().targetFps, encConfig.fps,
                adaptation.lastDecision().fecCapPct,
                fpsThrottler.policyDroppedCount(),
                frameQueue.pushDropCount() + frameQueue.consumerDropCount());
```

- [ ] **Step 7: 构建 + 全量测试**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -5 && ctest --test-dir build --output-on-failure 2>&1 | tail -12`
Expected: 构建成功（crystal_client 链接 crystal_media_adaptive），ctest 全绿（7 套件）

- [ ] **Step 8: 提交**

```bash
git add main_client.cpp CMakeLists.txt
git commit -m "feat(adaptive): main_client 集成自适应控制器与[net]指标行"
```

---

### Task 5: Demo 6 网络自适应演示

**Files:**
- Modify: `main_demo.cpp`（新增 demoAdaptive + main 调用）
- Modify: `CMakeLists.txt`（根，crystal_demo 链接新库）

- [ ] **Step 1: crystal_demo 链接**

根 `CMakeLists.txt` crystal_demo 目标（L42-48）的 whole-archive 组追加 `crystal_media_adaptive`：

```cmake
target_link_libraries(crystal_demo PRIVATE
    -Wl,--whole-archive
    crystal_media_video crystal_media_rtp crystal_utils crystal_media_gcc
    crystal_media_adaptive
    -Wl,--no-whole-archive
    spdlog::spdlog
    avcodec swresample swscale avutil
)
```

- [ ] **Step 2: 实现 Demo 6**

`main_demo.cpp` 中 `demoGcc()` 结束（`return 0;` 之后、`main()` 之前）插入：

```cpp
// ============================================================================
// Demo 6: 网络自适应（虚拟网络序列）
// ============================================================================
// 目的：无需网络/硬件，纯内存验证 分级去抖 → 跨流决策 的完整行为。
//
// 虚拟时间线（每 tick 200ms）：
//   第 1 段 (tick 0-9)  ：网络平稳  ratio=1.0, loss=0.5%, rtt=40ms → Good
//   第 2 段 (tick 10-24)：带宽砍半  ratio=0.5,  loss=8%,   rtt=250ms
//                         → 连续 3 tick 降级 → Poor（fps 30→15）
//   第 3 段 (tick 25-39)：WiFi 切换 ratio=0.25, loss=20%,  rtt=900ms
//                         → 连续 3 tick 降级 → Bad（fps→10，PLI 一次）
//   第 4 段 (tick 40-79)：网络恢复  → 连续 10 tick 升级 → Good
//                         （帧率阶梯 2s 一级：10→12→15→20→30）
//
// 预期观察：快降（3 tick）慢升（10 tick）的不对称；帧率阶梯逐级
// 恢复而非一步跳回；Bad 进入瞬间 PLI 只触发一次。
// ============================================================================
static int demoAdaptive() {
    printSeparator("Demo 6: 网络自适应（虚拟网络序列）");

    crystal::AdaptationController adaptation(1000, 30);  // 配置 1000kbps/30fps
    crystal::NetworkQuality lastLv = crystal::NetworkQuality::Good;
    uint32_t lastFps = 30;

    std::cout << "\n  模拟 80 个 tick（200ms/tick，共 16s）的网络时间线:\n"
              << "    1-10: 平稳 | 11-25: 带宽砍半 | 26-40: WiFi切换 | 41-80: 恢复\n\n";

    std::cout << "  " << std::left << std::setw(5) << "tick"
              << std::setw(9) << "gcc"
              << std::setw(7) << "loss%"
              << std::setw(7) << "rtt"
              << std::setw(7) << "等级"
              << std::setw(6) << "fps"
              << std::setw(8) << "fecCap"
              << "动作\n";
    std::cout << "  " << std::string(52, '-') << "\n";

    for (int t = 0; t < 80; ++t) {
        crystal::NetworkSignals sig;
        sig.configuredKbps = 1000;
        if (t < 10) {            // 平稳
            sig.gccTargetKbps = 1000; sig.lossPct = 0.5; sig.rttMs = 40;
        } else if (t < 25) {     // 带宽砍半
            sig.gccTargetKbps = 500;  sig.lossPct = 8;   sig.rttMs = 250;
        } else if (t < 40) {     // WiFi 切换（断崖）
            sig.gccTargetKbps = 250;  sig.lossPct = 20;  sig.rttMs = 900;
        } else {                 // 恢复
            sig.gccTargetKbps = 1000; sig.lossPct = 0.5; sig.rttMs = 40;
        }

        auto d = adaptation.tick(sig, static_cast<uint64_t>(t) * 200);

        // 只打印关键行（等级变化/PLI/阶梯移动/每 5 tick），避免刷屏
        std::string action = d.requestPli ? "PLI→" : "";
        bool interesting = (d.level != lastLv) || (d.targetFps != lastFps)
                           || d.requestPli || (t % 5 == 0);
        if (interesting) {
            std::cout << "  " << std::left << std::setw(5) << t
                      << std::setw(9) << sig.gccTargetKbps
                      << std::setw(7) << sig.lossPct
                      << std::setw(7) << static_cast<int>(sig.rttMs)
                      << std::setw(7) << crystal::networkQualityName(d.level)
                      << std::setw(6) << d.targetFps
                      << std::setw(8) << d.fecCapPct
                      << action << "\n";
        }
        lastLv = d.level;
        lastFps = d.targetFps;
    }

    std::cout << "\n  [观察结论]\n";
    std::cout << "  1. 带宽砍半后 3 个 tick（600ms）降级 Poor：fps 30→15\n";
    std::cout << "  2. WiFi 切换后 3 tick 降级 Bad：fps→10，PLI 触发一次\n";
    std::cout << "  3. 恢复后 10 个 tick（2s）才升级：帧率阶梯 2s 一级爬回\n";
    std::cout << "  4. 快降慢升不对称 = 防乒乓（AIMD 同款哲学）\n";
    std::cout << "  5. Bad 期 fecCap 收紧到 30%：防 FEC 挤占视频载荷\n";

    return 0;
}
```

main() 中 `demoGcc();` 之后追加调用：

```cpp
    demoAdaptive();       // Demo 6: 网络自适应（虚拟网络序列）
```

总结打印（`5. GCC带宽估计...` 之后）追加一行：

```cpp
    std::cout << "  6. 网络自适应:  分级→降帧率/FEC上限/PLI 跨流联动\n\n";
```

注意：`static` 局部变量在 demo 中只跑一次没问题；若 main_demo.cpp 头部没有 `#include "media/adaptive/adaptation_controller.h"` 则补上（demoGcc 已含 gcc_controller.h，风格一致）。

- [ ] **Step 3: 构建 + 运行 demo**

Run: `cmake --build build -j"$(nproc)" --target crystal_demo && timeout 60 ./build/crystal_demo 2>&1 | tail -30`
Expected: Demo 1-6 依次输出，Demo 6 可见等级切换 tick 序列（Good→Poor at ~t12, Bad at ~t27, 恢复爬升）与观察结论

- [ ] **Step 4: 提交**

```bash
git add main_demo.cpp CMakeLists.txt
git commit -m "feat(demo): Demo 6 网络自适应虚拟演示"
```

---

### Task 6: LEARNING_GUIDE.md（Module 14 + TOC + Module 0 + 面试专题）

**Files:**
- Modify: `docs/LEARNING_GUIDE.md`

**结构定位（已核实）：**
- 现有模块序：0-10 → 13（可观测性, L2776）→ 11（GCC, L2861）→ 12（线程, L3075）→ 面试专题（L3219）→ 文件尾（~L3396）
- TOC 在文件头 L12-24，**Module 12 缺失**（顺带修复）
- Module 0 线程模型在 L176

- [ ] **Step 1: TOC 更新**

TOC 中 Module 11 条目后追加两行（修复 12 缺失 + 新增 14）：

```markdown
- [Module 12：线程模型与无锁编程【工程化升级 v2/Phase D 新增】](#module-12线程模型与无锁编程工程化升级-v2phase-d-新增)
- [Module 14：网络自适应与降级策略【工程化升级 v2/Phase E 新增】](#module-14网络自适应与降级策略工程化升级-v2phase-e-新增)
```

- [ ] **Step 2: Module 0 线程模型补自适应节点**

L176 `#### 线程模型（面试重点）` 小节内，主循环职责列表中 GCC tick 描述旁追加一条（保持原格式风格）：

```markdown
- 主循环（200ms 节拍）：网络自适应 tick——收集 GCC 目标码率/RTT/丢包率
  → AdaptationController 分级与决策（帧率钳制/FEC 上限/PLI），
  经原子交接给编码线程（FramerateThrottler）与音频采集线程（Opus FEC）
```

并在 Module 0 的架构描述文字中（信号流向列表）补一句：GCC 产出目标码率后，AdaptationController 消费它做跨流策略（详见 Module 14）。

- [ ] **Step 3: 新增 Module 14（插在 Module 12 结束、面试专题 L3219 之前）**

完整章节内容（老师级中文注释风格，对齐 Module 11/12 的结构）：

````markdown
## Module 14：网络自适应与降级策略【工程化升级 v2/Phase E 新增】

（内容要点——按既有模块"原理图 → 逐组件精读 → Demo → 面试高频题 → 生产差异"结构展开：）

1. **为什么 GCC 不够**：GCC 只回答"带宽是多少"，没回答"帧率降多少/
   FEC 编多少/要不要关键帧"——跨流策略是 Call 级职责
   （libwebrtc NetworkController/ResourceAdaptationProcessor 的定位）
2. **分层图**：GCC（估计）→ AdaptationController（策略）→ 执行器
   （FramerateThrottler / opusLossPct_ / sendPli）三级，与 Module 11/12
   的线程拓扑图衔接
3. **分级器精读**：`network_quality_classifier.h` 级联判定防二义性、
   ratio 的含义（GCC 能给的/编码器想要的）、RTT 未知不参与判级
4. **去抖状态机**：快降（3 tick/600ms）慢升（10 tick/2s）的不对称设计，
   candidate streak 防乒乓——画时间轴图讲解
5. **帧率阶梯**：降级跳档/升级逐级+minDwell；为什么丢帧不重配编码器
   （degradation preference：MaintainFramerate/MaintainResolution/
   Balanced 三模式对比，本实现 = MaintainResolution 路径）
6. **FEC 上限曲线**：40/50/30% 的 protection overhead 权衡——
   坏网络里过度保护挤占视频有效载荷
7. **PLI edge 语义**：进入 Bad 一次 + sendPli 500ms 节流双保险
8. **连接状态监控**：pc->state() 轮询 vs 回调的取舍；libdatachannel
   封装 ICE 的边界；selected candidate pair 为什么拿不到（getStats 视角）
9. **Demo 6 对照**：虚拟时间线四段的输出解读
10. **面试高频题**（追问链形式）：
    - 为什么升级慢降级快？如果反过来会怎样？
    - 帧率降级与分辨率降级的实现路径差异？各自适用场景？
    - FEC 冗余度为什么有上限？protection overhead 怎么算？
    - 没有 RTT 数据时怎么分级？（本实现：视为不命中）
    - 去抖的 streak 计数为什么能防乒乓？
11. **生产差异**：libwebrtc ResourceAdaptationProcessor/VideoStreamAdapter
    与本实现的对应关系；没做的：分辨率降级（ReconfigureEncoder 重路径）、
    基于卡顿的体验驱动降级、跨流带宽分配、Simulcast/SVC
````

（执行者按上述要点展开成完整正文，风格对照 Module 11/12：原理段落 + 表格 + 代码走读引用 + 面试题问答）

- [ ] **Step 4: 面试专题追加追问链**

面试专题章节（L3219 起）末尾追加小节：

```markdown
### 网络自适应追问链（Phase E 新增）

Q: 你们网络变差时具体做了什么？
A: GCC 目标码率联动编码器只是第一层；之上还有网络分级器
   （ratio+RTT+丢包率四级），分级驱动帧率钳制（编码前策略丢帧，
   不重配编码器）、Opus FEC 冗余度上限（protection overhead）和
   PLI 关键帧请求——快降慢升防乒乓。

Q: 为什么丢帧不降分辨率？
A: 重配 x264 需要重建上下文，重配瞬间可能卡顿；丢帧是零成本降带宽。
   libwebrtc 的 degradation preference 里 MaintainResolution 模式
   就是这条路径，Balanced 才会两者都动。

Q: 分级为什么要去抖？
A: 阈值附近震荡会引发帧率/码率乒乓，体验比稳定差网更糟。
   降级 600ms 生效、升级 2s 生效——不对称，AIMD 同款哲学。
```

- [ ] **Step 5: 提交**

```bash
git add docs/LEARNING_GUIDE.md
git commit -m "docs: Module 14 网络自适应与降级策略 + TOC/面试专题更新"
```

---

### Task 7: 全量构建 + ctest 全绿 + demo 回归

**Files:** 无新文件（验证任务）

- [ ] **Step 1: 全量构建**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -8`
Expected: 100% Built target crystal_client / crystal_demo / crystal_adaptive_tests，无警告错误

- [ ] **Step 2: ctest 全绿**

Run: `ctest --test-dir build --output-on-failure 2>&1 | tail -12`
Expected: 7/7 套件 PASS（新增 crystal_adaptive_tests）

- [ ] **Step 3: 新测试细节确认**

Run: `./build/tests/crystal_adaptive_tests`
Expected: 22 个用例全 PASS（分类器 10 + 控制器 8 + 节流器 4）

- [ ] **Step 4: demo 回归**

Run: `timeout 60 ./build/crystal_demo 2>&1 | tail -12; echo EXIT=$?`
Expected: EXIT=0，Demo 1-6 完整输出（含 Demo 6 观察结论与总结行"6. 网络自适应"）

- [ ] **Step 5: 客户端冒烟（无硬件环境）**

Run: `timeout 5 ./build/crystal_client --room test 2>&1 | tail -5; echo EXIT=$?`
Expected: 正常启动并输出日志后超时退出（EXIT=124）；无 crash、无链接错误。摄像头/麦克风缺失告警属预期。

- [ ] **Step 6: 提交收尾（如仍有未提交文件）**

```bash
git status --short   # 确认无遗漏
git log --oneline -8  # 确认提交序列
```
