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
