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

// E2E：SR 锚点 + RTP 时间差 + RTT/2，取 5s 窗口最小值
TEST(MetricsCollector, E2eDelay) {
    crystal::MetricsCollector m(90000, 30);   // 视频时钟 90kHz
    m.setRttMs(100.0);                        // RTT 100ms → RTT/2 = 50ms
    // 对端 SR 在发送侧 t=0 发出（RTP 锚点 ts=0），网络单向 50ms，本地 t=50 到达
    m.onSenderReportMapping(0, 50);

    // 包 A：发送侧 t=100 发出（RTP ts=9000，90kHz 下 9000/90=100ms），t=150 到达本地
    //   e2e = (150 - 50) - 100 + 50 = 50ms ✓（= 真实单向延迟）
    m.onPacketArrival(150, 9000);
    ASSERT_TRUE(m.hasE2e());
    EXPECT_NEAR(m.e2eDelayMs(), 50.0, 0.01);

    // 包 B：网络抖动导致 80ms 单向才到（排队 30ms）
    m.onPacketArrival(280, 18000);            // 发送侧 t=200，到达 t=280
    EXPECT_NEAR(m.e2eDelayMs(), 50.0, 0.01);  // min 仍是 50（B 是 80）

    // RTP 时间戳回绕：32 位回绕点 2^32-1 附近（音频 48kHz 流）
    crystal::MetricsCollector m2(48000, 0);
    m2.onSenderReportMapping(4294967295u, 1000);   // 锚点 = 2^32 - 1
    m2.setRttMs(80.0);                             // RTT/2 = 40ms
    // 音频 48kHz：锚点后 480 采样 = 10ms；包 ts = (2^32-1) + 480 mod 2^32 = 479
    //   int32 差 = +480（回绕正确处理），e2e = (1050-1000) - 10 + 40 = 80ms
    m2.onPacketArrival(1050, 479);
    ASSERT_TRUE(m2.hasE2e());
    EXPECT_NEAR(m2.e2eDelayMs(), 80.0, 0.01);
}

// 拼接行：视频流含 fps/卡顿段，音频流不含
TEST(MetricsCollector, SummaryLine) {
    crystal::MetricsCollector video(90000, 30);
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
