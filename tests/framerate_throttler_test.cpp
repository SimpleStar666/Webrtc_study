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
