// ============================================================================
// gcc_controller_test.cpp — GCC 控制器测试（v2 新增）
// ============================================================================
// 双通道融合：丢包通道（RR）优先级硬规则，趋势通道（TWCC→trendline）做
// 精细化升降。AIMD：过载 ×0.85，正常递增，钳制 [100,4000] kbps。
// ============================================================================

#include <gtest/gtest.h>
#include "media/gcc/gcc_controller.h"

// 初始码率取构造值
TEST(Gcc, InitialBitrate) {
    crystal::GccController gcc(1000);
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
}

// 丢包通道：>10% 强降（无视趋势通道）
TEST(Gcc, LossAbove10Reduces) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.15);
    EXPECT_LT(gcc.targetBitrateKbps(), 1000u);
    EXPECT_NEAR(gcc.targetBitrateKbps(), 850u, 1);  // ×0.85
}

// 丢包通道：2%~10% 持平
TEST(Gcc, LossMidRangeHold) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.05);
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
}

// 丢包通道：<2% 允许递增
TEST(Gcc, LossBelow2AllowsIncrease) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.01);
    EXPECT_GT(gcc.targetBitrateKbps(), 1000u);
}

// 趋势通道：构造延迟持续增长（每 feedback 一批样本）→ 连续 3 轮过载才降（去抖）
TEST(Gcc, OveruseTrendlineReduces) {
    crystal::GccController gcc(1000);
    // 每轮 20 个样本：发送间隔 10ms，OWD 每包 +0.5ms（斜率 ≈0.048 > 阈值 0.01）
    for (int round = 0; round < 3; ++round) {
        crystal::FeedbackSample s;
        s.arrivals.reserve(20);
        s.owdMs.reserve(20);
        for (int i = 0; i < 20; ++i) {
            double send = round * 200.0 + i * 10.0;
            double owd = 200.0 + (round * 20 + i) * 0.5;  // 排队延迟持续增长
            s.arrivals.push_back({static_cast<uint16_t>(100 + round * 20 + i),
                                  send + owd});
            s.owdMs.push_back(owd);
        }
        gcc.onFeedback(s);
        gcc.tick();  // 每轮结束应用状态机
    }
    EXPECT_LT(gcc.targetBitrateKbps(), 1000u);
}

// 趋势通道：延迟平稳 → 不降（斜率 ≈0，streak 不增长）
TEST(Gcc, FlatTrendlineHolds) {
    crystal::GccController gcc(1000);
    for (int round = 0; round < 4; ++round) {
        crystal::FeedbackSample s;
        for (int i = 0; i < 20; ++i) {
            double send = round * 200.0 + i * 10.0;
            s.arrivals.push_back({static_cast<uint16_t>(100 + round * 20 + i),
                                  send + 200.0});
            s.owdMs.push_back(200.0);
        }
        gcc.onFeedback(s);
        gcc.tick();
    }
    // 平稳网络下反而按 AIMD 递增
    EXPECT_GT(gcc.targetBitrateKbps(), 1000u);
}

// 钳制下限 100kbps
TEST(Gcc, ClampsToMinimum) {
    crystal::GccController gcc(150);
    for (int i = 0; i < 10; ++i) gcc.onLossUpdate(0.5);
    EXPECT_EQ(gcc.targetBitrateKbps(), 100u);
}

// 钳制上限 4000kbps
TEST(Gcc, ClampsToMaximum) {
    crystal::GccController gcc(3900);
    for (int i = 0; i < 30; ++i) gcc.onLossUpdate(0.0);
    EXPECT_EQ(gcc.targetBitrateKbps(), 4000u);
}

// 过载嫌疑期（斜率超阈值但 streak 未满 3 次去抖）：保持码率不增长
// （嫌疑期继续加码只会把队列压得更满；正确行为是保持-观察）
TEST(Gcc, HoldDuringOveruseSuspicion) {
    crystal::GccController gcc(1000);
    auto feedOveruse = [&](int base) {   // 20 样本/轮，OWD 持续增长
        crystal::FeedbackSample s;
        for (int i = 0; i < 20; ++i) {
            double send = base * 10.0;
            double owd = 100.0 + (base + i) * 0.5;
            s.arrivals.push_back({static_cast<uint16_t>(base + i), send + owd});
            s.owdMs.push_back(owd);
        }
        gcc.onFeedback(s);
    };
    feedOveruse(0);
    gcc.tick();                            // streak 1：嫌疑期 → 保持
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
    feedOveruse(20);
    gcc.tick();                            // streak 2：仍保持
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
    feedOveruse(40);
    gcc.tick();                            // streak 3 → ×0.85 退避
    EXPECT_NEAR(gcc.targetBitrateKbps(), 850u, 1);
}
