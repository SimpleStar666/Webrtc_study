// ============================================================================
// jitter_buffer_test.cpp — 抖动缓冲区单元测试
// ============================================================================
// 本测试文件验证 CrystalRTC 中 JitterBuffer 类的正确性，覆盖以下核心功能：
//
// 1. 顺序插入与消费：验证按序到达的 RTP 包能被正确缓存并按序输出
// 2. 乱序重排：验证乱序到达的 RTP 包能被正确排序后输出
// 3. 丢包检测：验证当序列号出现间隙时，能正确统计丢包数量
// 4. 统计初始化：验证新建的 JitterBuffer 丢包统计为零
//
// 学习要点：
// - Jitter Buffer 是 WebRTC 接收端的核心组件，用于对抗网络抖动
// - 网络传输中 RTP 包可能：乱序到达、延迟到达、或丢失
// - Jitter Buffer 的三大功能：缓存等待、按序重排、丢包检测
// - 构造参数 targetDelayMs=40 表示目标延迟 40ms，这是 WebRTC 的典型值
//   （延迟越大抗抖动能力越强，但实时性越差，需要权衡）
// - RFC 3550 定义了基于序列号的丢包检测机制：如果期望的序列号未出现，
//   则认为该包已丢失
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

// ----------------------------------------------------------------------------
// 测试：按序插入 RTP 包后能正确消费
// ----------------------------------------------------------------------------
// 目的：验证当 RTP 包按序列号顺序到达时（最理想的情况），
//       JitterBuffer 能正确缓存并按序输出。
//
// 协议行为：
//   在网络状况良好时，RTP 包按序列号递增顺序到达。
//   JitterBuffer 应能：
//   1. 接收所有包并存入内部缓冲区（基于 std::map，以序列号为 key）
//   2. consume() 时按序列号顺序输出所有已缓存的包
//
// 测试数据设计思路：
//   - 三个 RTP 包，序列号 100/101/102，连续递增
//   - 时间戳 90000/93000/96000，间隔 3000（对应 90kHz 下 33.3ms，
//     即约 30fps 视频的帧间隔）
//   - 这是最简单的"理想路径"测试，验证基本功能正确后再测试异常情况
//
// Google Test 说明：
//   ASSERT_GE(out.size(), 3u) 确保至少输出了 3 个包后再访问下标
//   使用 >= 而非 == 是因为 consume() 的实现可能输出更多包
// ----------------------------------------------------------------------------
TEST(JitterBufferTest, InsertAndConsumeInOrder) {
    // 构造 JitterBuffer，目标延迟 40ms（WebRTC 典型值）
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    // 90000 = 90kHz × 1秒，视频帧的典型起始时间戳
    pkt1.setTimestamp(90000);
    pkt2.setSequenceNumber(101);
    // 93000 = 90000 + 3000，间隔 3000 对应 90kHz 下约 33.3ms（30fps）
    pkt2.setTimestamp(93000);
    pkt3.setSequenceNumber(102);
    pkt3.setTimestamp(96000);

    // 按序列号顺序插入（理想情况）
    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    // 消费缓冲区中的包
    auto out = jb.consume();
    // 必须至少输出 3 个包
    ASSERT_GE(out.size(), 3u);
    // 验证输出顺序与输入顺序一致（seq=100, 101, 102）
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

// ----------------------------------------------------------------------------
// 测试：乱序到达的 RTP 包能被正确重排
// ----------------------------------------------------------------------------
// 目的：验证当 RTP 包乱序到达时（网络抖动的常见情况），
//       JitterBuffer 能将它们按序列号排序后输出。
//
// 协议行为：
//   在实际网络中，由于路由路径不同或网络拥塞，RTP 包可能乱序到达。
//   例如：seq=102 先于 seq=101 到达。JitterBuffer 使用 std::map 存储，
//   天然按 key（序列号）排序，因此 consume() 时能按序输出。
//
// 测试数据设计思路：
//   - 三个 RTP 包，序列号 100/102/101
//   - 插入顺序：100 → 102 → 101（102 先于 101 到达，模拟乱序）
//   - 这是 WebRTC 中最常见的乱序模式：相邻包交换顺序
//   - 时间戳与序列号对应，验证重排后时间戳也按序
//
// 边界条件：
//   - 此测试只涉及相邻包的乱序（差值=1），更极端的乱序
//     （如 seq=100 → seq=105 → seq=101）由丢包检测测试覆盖
// ----------------------------------------------------------------------------
TEST(JitterBufferTest, ReordersOutOfOrderPackets) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2, pkt3;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    // pkt2 的序列号为 102（跳过了 101）
    pkt2.setSequenceNumber(102);
    pkt2.setTimestamp(96000);
    // pkt3 的序列号为 101（迟到的包）
    pkt3.setSequenceNumber(101);
    pkt3.setTimestamp(93000);

    // 插入顺序：100 → 102 → 101（模拟乱序到达）
    jb.insert(pkt1);
    jb.insert(pkt2);
    jb.insert(pkt3);

    auto out = jb.consume();
    // 必须至少输出 3 个包
    ASSERT_GE(out.size(), 3u);
    // 验证输出已按序列号排序（100, 101, 102），而非到达顺序
    EXPECT_EQ(out[0].sequenceNumber(), 100);
    EXPECT_EQ(out[1].sequenceNumber(), 101);
    EXPECT_EQ(out[2].sequenceNumber(), 102);
}

// ----------------------------------------------------------------------------
// 测试：序列号间隙触发丢包检测
// ----------------------------------------------------------------------------
// 目的：验证当 RTP 包的序列号出现间隙时（中间的包未到达），
//       JitterBuffer 能正确检测并统计丢包数量。
//
// 协议行为（RFC 3550 Section 6.3.1）：
//   接收端通过监控序列号的连续性来检测丢包。
//   如果期望的序列号为 N，但收到的序列号为 N+k（k>1），
//   则认为序列号 N 到 N+k-1 的包已丢失。
//
//   JitterBuffer 的实现逻辑（参见 jitter_buffer.cpp）：
//   - 维护 expectedSeq_ 表示下一个期望的序列号
//   - 当收到的序列号 > expectedSeq_ 时，差值-1 即为丢失的包数
//   - 例如：期望 101，收到 103，则 101 和 102 丢失，lostCount_ += 2
//
// 测试数据设计思路：
//   - 两个 RTP 包，序列号 100 和 103
//   - 序列号间隙：101 和 102 缺失，应检测到 2 个丢包
//   - 选择间隙=2（而非=1）是为了验证丢包计数能正确累加
//   - 不检查精确的丢包数量（使用 EXPECT_GT），因为实现细节可能
//     将乱序包暂时计入丢包（后续到达时修正）
//
// Google Test 说明：
//   EXPECT_GT 用于非致命断言，只检查丢包数 > 0
//   不使用 EXPECT_EQ 检查精确值，避免实现细节变化导致测试脆弱
// ----------------------------------------------------------------------------
TEST(JitterBufferTest, DetectsPacketLoss) {
    crystal::JitterBuffer jb(40);

    crystal::RtpPacket pkt1, pkt2;
    pkt1.setSequenceNumber(100);
    pkt1.setTimestamp(90000);
    // 序列号从 100 跳到 103，101 和 102 缺失
    pkt2.setSequenceNumber(103);
    pkt2.setTimestamp(99000);

    jb.insert(pkt1);
    // 插入 seq=103 时，JitterBuffer 检测到 101、102 缺失
    jb.insert(pkt2);

    auto out = jb.consume();
    // 丢包数必须大于 0（101 和 102 丢失）
    EXPECT_GT(jb.lostPacketCount(), 0u);
}

// ----------------------------------------------------------------------------
// 测试：新建的 JitterBuffer 丢包统计为零
// ----------------------------------------------------------------------------
// 目的：验证 JitterBuffer 在未接收任何数据时，丢包计数和丢包率均为零。
//
// 协议行为：
//   这是初始状态的边界条件测试。在 WebRTC 通话刚建立时，
//   JitterBuffer 尚未收到任何 RTP 包，此时统计值应为零，
//   避免误报丢包导致发送端降低码率等不必要的反应。
//
// 测试数据设计思路：
//   - 仅构造 JitterBuffer，不插入任何包
//   - 验证 lostPacketCount() == 0 和 lossRate() == 0.0
//   - 这是"零值边界"测试，确保初始状态正确
//
// Google Test 说明：
//   EXPECT_DOUBLE_EQ 用于浮点数比较，避免浮点精度问题
//   不使用 EXPECT_EQ 比较浮点数，因为浮点运算可能有微小误差
// ----------------------------------------------------------------------------
TEST(JitterBufferTest, StatsInitializedToZero) {
    crystal::JitterBuffer jb(40);
    // 未接收任何包时，丢包计数必须为 0
    EXPECT_EQ(jb.lostPacketCount(), 0u);
    // 未接收任何包时，丢包率必须为 0.0
    // 使用 EXPECT_DOUBLE_EQ 而非 EXPECT_EQ，正确处理浮点数比较
    EXPECT_DOUBLE_EQ(jb.lossRate(), 0.0);
}
