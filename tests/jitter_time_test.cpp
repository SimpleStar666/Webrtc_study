// ============================================================================
// jitter_time_test.cpp — JitterBuffer 时间驱动释放测试（工程化升级 v2 Phase D）
// ============================================================================
//
// 本测试文件验证 JitterBuffer::consume(nowMs) 的时间驱动语义与
// 自适应生效延迟，覆盖四个核心行为：
//
// 1. 等待不足不释放：缺口包未等够 effectiveDelay → 按兵不动
//    （迟到包可能马上就到，等它值得）
// 2. 等够释放：距队首包到达 ≥ effectiveDelay → 缺口判丢，越过输出
//    （延迟有上界，这是对旧版"大间隙无限等"缺陷的修复）
// 3. 迟到包在等待期内到达：缺口被补上 → 无空洞按序输出
//    （时间驱动等待的全部意义：给迟到的包留窗口）
// 4. 自适应延迟：持续乱序 → effectiveDelayMs() 加深；网络恢复 → 衰减回基础值
// 5. 向后兼容：无参 consume() 保持旧版序号启发式（gap≤3 立即越过）
//
// 【可测性设计】时间由调用方注入（setArrivalMs / consume(nowMs)），
// 类内不读系统时钟——测试可以构造任意时间序列精确控制"等了多久"。
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

namespace {

// 构造辅助：一个带序号、时间戳、到达时刻的最小 RTP 包
crystal::RtpPacket makePkt(uint16_t seq, uint32_t rtpTs, uint64_t arrivalMs) {
    crystal::RtpPacket p;
    p.setSequenceNumber(seq);
    p.setTimestamp(rtpTs);
    p.setArrivalMs(arrivalMs);
    return p;
}

} // namespace

// ----------------------------------------------------------------------------
// 测试：等待不足不释放
// ----------------------------------------------------------------------------
// 场景：seq=100 到达后，seq=102 直接到达（101 缺口），基础延迟 40ms。
// consume 的时刻距缺口后队首包（102）到达：
//   1020 → 等了 16ms < 40ms → 102 按兵不动（101 可能马上就到）
//   1030 → 等了 26ms < 40ms → 继续等
// 100 在缺口之前，属于"已到期"，照常输出（100 与 102 的缺口在 101）。
TEST(JitterTimeTest, InsufficientWaitHoldsBack) {
    crystal::JitterBuffer jb(40);  // 基础延迟 40ms

    jb.insert(makePkt(100, 90000, 1000));
    jb.insert(makePkt(102, 96000, 1004));  // 101 缺失

    auto out = jb.consume(1020);  // 102 只等了 16ms
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);  // 缺口前的包正常输出

    out = jb.consume(1030);  // 等了 26ms，仍不足 40ms
    EXPECT_TRUE(out.empty());  // 102 继续等待——这就是"给迟到包留窗口"
}

// ----------------------------------------------------------------------------
// 测试：等够生效延迟后释放（延迟有界）
// ----------------------------------------------------------------------------
// 同一场景推进时间：等到 1045ms，102 已等 41ms ≥ 40ms → 缺口判丢，
// 102 越过输出。旧版行为对照：gap=1≤3 立即放行（不看时间）；
// 时间驱动的增益在 gap>3 的场景——真丢包时等待有上界，不会无限卡住。
TEST(JitterTimeTest, ReleasesAfterEffectiveDelay) {
    crystal::JitterBuffer jb(40);

    jb.insert(makePkt(100, 90000, 1000));
    jb.insert(makePkt(102, 96000, 1004));

    jb.consume(1030);  // 消费掉 100，102 等待中

    auto out = jb.consume(1045);  // 102 已等 41ms ≥ 40ms
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].sequenceNumber(), 102);
}

// ----------------------------------------------------------------------------
// 测试：等待窗口内迟到包到达，缺口补上 → 无空洞按序输出
// ----------------------------------------------------------------------------
// 时间驱动等待的全部意义：101 在 102 放行之前到达，缺口消失，
// 101、102 作为连续序列一起输出——不需要 NACK 重传就自愈了。
TEST(JitterTimeTest, LatePacketFillsGapDuringWait) {
    crystal::JitterBuffer jb(40);

    jb.insert(makePkt(100, 90000, 1000));
    jb.insert(makePkt(102, 96000, 1004));
    jb.consume(1010);  // 100 输出，102 等待中（等了 6ms）

    // 101 在等待窗口内（< 40ms）迟到到达
    jb.insert(makePkt(101, 93000, 1012));

    auto out = jb.consume(1015);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].sequenceNumber(), 101);  // 连续、有序、无空洞
    EXPECT_EQ(out[1].sequenceNumber(), 102);
}

// ----------------------------------------------------------------------------
// 测试：未打点的包视作"已等待足够久"立即放行
// ----------------------------------------------------------------------------
// RtpPacket.arrivalMs 默认 0：调用方未注入时钟（如存量代码路径）时，
// 时间驱动模式退化为"缺口直接越过"——不会卡死缓冲区。
TEST(JitterTimeTest, UnstampedPacketTreatedAsFullyWaited) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket a, b;  // 不 setArrivalMs
    a.setSequenceNumber(100);
    a.setTimestamp(90000);
    b.setSequenceNumber(105);  // 缺口 4 个包
    b.setTimestamp(105000);
    jb.insert(a);
    jb.insert(b);

    auto out = jb.consume(1000);  // 任意时刻
    ASSERT_EQ(out.size(), 2u);    // arrival=0 → waited 巨大 → 直接放行
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 105);
}

// ----------------------------------------------------------------------------
// 测试：自适应生效延迟——持续乱序加深，恢复后衰减
// ----------------------------------------------------------------------------
// 交换相邻对（100,102,101,103,105,104,...）制造持续深度 2 的乱序：
//   乱序深度 EMA（增益 1/4）向 2 收敛
//   effectiveDelay = 40 + 深度 × 包间隔 > 40（加深）
// 之后连续 12 个在序包：深度向 0 衰减 → effectiveDelay 回落（< 加深值）。
// 上限 200ms 在 jitter_buffer.h 的 kMaxEffectiveDelayMs 兜底。
TEST(JitterTimeTest, AdaptiveDelayDeepensThenDecays) {
    crystal::JitterBuffer jb(40);

    // ---- 阶段1：持续乱序（每对交换，深度 2）----
    uint64_t t = 1000;
    uint16_t seq = 100;
    for (int i = 0; i < 8; ++i) {
        jb.insert(makePkt(static_cast<uint16_t>(seq + 1), 90000 + 60 * i, t));
        jb.insert(makePkt(seq, 90000 + 30 + 60 * i, t));  // 迟到的偶数包
        seq = static_cast<uint16_t>(seq + 2);
        t += 20;
    }
    uint32_t deepened = jb.effectiveDelayMs();
    EXPECT_GT(deepened, 40u) << "持续乱序应加深生效延迟";
    EXPECT_LE(deepened, 200u) << "加深有硬上限（kMaxEffectiveDelayMs）";

    // ---- 阶段2：网络恢复，连续在序包 ----
    for (int i = 0; i < 12; ++i) {
        jb.insert(makePkt(seq++, 90000 + 500 + 30 * i, t));
        t += 20;
    }
    uint32_t recovered = jb.effectiveDelayMs();
    EXPECT_LT(recovered, deepened) << "在序流应让乱序深度衰减";
}

// ----------------------------------------------------------------------------
// 测试：无参 consume() 向后兼容旧版序号启发式
// ----------------------------------------------------------------------------
// 旧规则：gap ≤ 3 立即越过输出；gap > 3 等待。
// 存量测试（jitter_buffer_test / jitter_buffer_gap_test）与 demo 依赖
// 此行为，时间驱动改造不破坏它们。
TEST(JitterTimeTest, LegacyConsumeBackwardCompatible) {
    crystal::JitterBuffer jb(40);

    // 小缺口（=3）：立即越过
    jb.insert(makePkt(100, 90000, 1000));
    jb.insert(makePkt(104, 102000, 1002));
    auto out = jb.consume();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 104);

    // 大缺口（=9）：旧版无限等（这正是时间驱动要修的缺陷）
    crystal::JitterBuffer jb2(40);
    jb2.insert(makePkt(200, 120000, 2000));
    jb2.insert(makePkt(210, 150000, 2002));
    auto out2 = jb2.consume();
    ASSERT_EQ(out2.size(), 1u);
    EXPECT_EQ(out2[0].sequenceNumber(), 200);  // 210 还在等
}
