// ============================================================================
// trendline_test.cpp — Trendline 斜率估计器测试（v2 新增）
// ============================================================================
// 最小二乘线性回归：对 (到达时刻, 累积延迟) 样本求斜率。
// 斜率含义：每毫秒到达时间增加多少毫秒延迟（无量纲，>0 拥塞趋势）。
// ============================================================================

#include <gtest/gtest.h>
#include "media/gcc/trendline_estimator.h"

// 延迟线性增长：每 10ms 到达间隔累积 0.5ms 延迟 → 斜率 ≈ 0.05
TEST(Trendline, IncreasingDelayPositiveSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, i * 0.5);
    EXPECT_GT(est.slope(), 0.03);
    EXPECT_LT(est.slope(), 0.07);
}

// 延迟恒定：斜率 ≈ 0
TEST(Trendline, FlatDelayZeroSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, 5.0);
    EXPECT_NEAR(est.slope(), 0.0, 0.005);
}

// 延迟恢复（下降）：斜率 < 0
TEST(Trendline, DecreasingDelayNegativeSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, 10.0 - i * 0.4);
    EXPECT_LT(est.slope(), -0.02);
}

// 样本不足窗口：不应产出有效斜率（ready() = false）
TEST(Trendline, NotReadyBelowWindow) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 10; ++i)
        est.update(i * 10.0, i * 0.5);
    EXPECT_FALSE(est.ready());
}

// 窗口滑动：老样本滚出后斜率跟上新趋势
TEST(Trendline, SlidingWindow) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, i * 0.5);      // 先增长
    for (int i = 20; i < 40; ++i)
        est.update(i * 10.0, 10.0);          // 后平稳
    EXPECT_NEAR(est.slope(), 0.0, 0.02);
}
