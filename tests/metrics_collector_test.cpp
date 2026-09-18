// ============================================================================
// metrics_collector_test.cpp - 可观测性指标采集器单测（工程化升级 v2 新增）
// ============================================================================
//
// 【测试设计说明】
// 所有时间都由测试显式传入（构造时间序列），不依赖真实时钟——
// 这是可测性的关键：MetricsCollector 接收 uint64_t nowMs 参数而非
// 内部调 nowMs()，正是为了这一天。
//
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
    // 窗口推进到 [3500,8500)：33*106=3498 <3500 已逐出，实际剩 0 帧
    EXPECT_NEAR(m.renderFpsAt(9000), 0.0, 0.01);
}

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
