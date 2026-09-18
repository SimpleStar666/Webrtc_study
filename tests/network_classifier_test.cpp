// ============================================================================
// network_classifier_test.cpp — 网络分级器 + 自适应控制器测试（Phase E）
// ============================================================================
// 分级器是纯函数（无时钟无锁），直接构造 NetworkSignals 断言等级。
// 控制器测试注入时间序列（nowMs 由调用方传入），验证去抖/阶梯/PLI。
// ============================================================================
#include <gtest/gtest.h>
#include <algorithm>
#include <vector>
#include "media/adaptive/network_quality_classifier.h"
#include "media/adaptive/adaptation_controller.h"

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
