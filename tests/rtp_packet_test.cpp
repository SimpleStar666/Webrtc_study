// ============================================================================
// rtp_packet_test.cpp — RTP 数据包单元测试
// ============================================================================
// 本测试文件验证 CrystalRTC 中 RtpPacket 类的正确性，覆盖以下核心功能：
//
// 1. 默认构造函数：验证 RTP 包的初始状态符合 RFC 3550 规范
// 2. 字段读写：验证 Marker、Payload Type、Sequence Number、Timestamp、SSRC
//    等关键字段的 getter/setter 一致性
// 3. 序列化与反序列化：验证 RTP 包能正确地序列化为字节流，再从字节流
//    解析还原，这是 RTP 协议网络传输的基础
// 4. 头部大小与总大小：验证 RTP 固定头部为 12 字节（RFC 3550 规定），
//    以及总大小 = 头部 + 载荷
//
// 学习要点：
// - RTP (Real-time Transport Protocol) 是 WebRTC 的传输层协议
// - 每个 RTP 包由固定头部（12字节）+ 可选 CSRC + 可选扩展 + 载荷组成
// - version 字段固定为 2（RFC 3550），这是区分 RTP 与旧版 RFC 1890 的标志
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/rtp_packet.h"

// ----------------------------------------------------------------------------
// 测试：默认构造函数应产生符合 RFC 3550 的初始状态
// ----------------------------------------------------------------------------
// 目的：验证 RtpPacket 默认构造后，各字段处于协议规定的初始值。
// 协议行为：
//   - version 必须为 2（RFC 3550 规定 RTP 版本号为 2）
//   - padding 默认关闭（无填充）
//   - extension 默认关闭（无头部扩展）
//   - CSRC count 默认为 0（无贡献源）
//   - marker 默认关闭（非帧边界标记）
//   - payload type 默认为 0（未指定编码格式）
//   - sequence number 默认为 0（实际使用时应随机初始化，见 RFC 3550 5.1 节）
//   - timestamp 默认为 0（实际使用时应随机初始化）
//   - SSRC 默认为 0（实际使用时应随机生成）
//
// Google Test 说明：
//   EXPECT_EQ 用于非致命断言，即使失败也会继续执行后续检查
//   这适合本测试场景——我们希望一次检查所有字段的默认值
// ----------------------------------------------------------------------------
TEST(RtpPacketTest, DefaultConstructorHasVersion2) {
    crystal::RtpPacket pkt;
    // RTP 版本号必须为 2，这是 RFC 3550 的硬性规定
    EXPECT_EQ(pkt.version(), 2);
    // 默认无填充（padding=0），载荷末尾没有填充字节
    EXPECT_EQ(pkt.padding(), false);
    // 默认无头部扩展（extension=0）
    EXPECT_EQ(pkt.extension(), false);
    // 默认无 CSRC 标识符（csrc_count=0）
    EXPECT_EQ(pkt.csrcCount(), 0);
    // 默认 marker 位为 false，表示该包不是帧的最后一个包
    EXPECT_EQ(pkt.marker(), false);
    // 默认 payload type 为 0（实际应用中 0 对应 G.711 PCMU 音频编码）
    EXPECT_EQ(pkt.payloadType(), 0);
    // 默认序列号为 0（RFC 3550 建议初始序列号应随机化以增强安全性）
    EXPECT_EQ(pkt.sequenceNumber(), 0);
    // 默认时间戳为 0（RFC 3550 建议初始时间戳也应随机化）
    EXPECT_EQ(pkt.timestamp(), 0u);
    // 默认 SSRC 为 0（实际使用时应随机生成唯一的同步源标识符）
    EXPECT_EQ(pkt.ssrc(), 0u);
}

// ----------------------------------------------------------------------------
// 测试：字段的 setter/getter 一致性
// ----------------------------------------------------------------------------
// 目的：验证通过 setter 设置的值能通过 getter 正确读回。
// 协议行为：
//   - Marker 位：在视频 RTP 中，通常标记一帧的最后一个包，
//     接收端据此判断是否可以完整解码一帧
//   - Payload Type 96：动态分配的 PT 号（RFC 3551 规定 96-127 为动态类型），
//     在 WebRTC 中常用于 VP8/VP9/H264 等视频编码
//   - Sequence Number 12345：每个 RTP 包递增 1，用于检测丢包和重排序
//   - Timestamp 90000：视频常用 90kHz 时钟频率，90000 表示 1 秒的时间偏移
//   - SSRC 0xDEADBEEF：32 位同步源标识符，用于标识一个 RTP 流
//
// 测试数据设计思路：
//   - PT=96 选择了动态类型的起始值，是 WebRTC 视频的典型配置
//   - SSRC=0xDEADBEEF 选择了明显的特征值，便于调试时识别
//   - Timestamp=90000 对应 90kHz 时钟下的 1 秒，是视频帧间隔的常见值
// ----------------------------------------------------------------------------
TEST(RtpPacketTest, SetAndGetFields) {
    crystal::RtpPacket pkt;
    pkt.setMarker(true);
    // PT=96 是动态 payload type，WebRTC 中常用于 H.264/VP8 视频
    pkt.setPayloadType(96);
    pkt.setSequenceNumber(12345);
    // 90000 = 90kHz × 1秒，视频 RTP 的典型时钟频率
    pkt.setTimestamp(90000);
    // 使用特征明显的 SSRC 值，便于验证序列化/反序列化后的一致性
    pkt.setSsrc(0xDEADBEEF);

    // 验证 marker 位正确设置（帧结束标记）
    EXPECT_TRUE(pkt.marker());
    // 验证动态 payload type 正确存储
    EXPECT_EQ(pkt.payloadType(), 96);
    // 验证序列号正确存储
    EXPECT_EQ(pkt.sequenceNumber(), 12345);
    // 验证时间戳正确存储（u 后缀表示 unsigned，避免有符号比较警告）
    EXPECT_EQ(pkt.timestamp(), 90000u);
    // 验证 SSRC 正确存储
    EXPECT_EQ(pkt.ssrc(), 0xDEADBEEFu);
}

// ----------------------------------------------------------------------------
// 测试：序列化与反序列化的往返一致性（Round-trip Test）
// ----------------------------------------------------------------------------
// 目的：验证 RTP 包经过 serialize() → parse() 的完整往返后，
//       所有字段和载荷数据保持不变。这是网络传输的核心保证——
//       发送端序列化的字节流必须能被接收端正确解析。
//
// 协议行为：
//   - RTP 包在网络上以大端序（网络字节序）传输
//   - serialize() 将内存中的字段按 RFC 3550 格式编码为字节流
//   - parse() 从字节流中按协议格式解码各字段
//   - 两者必须互为逆操作
//
// 测试数据设计思路：
//   - Payload {0x00, 0x01, 0x02, 0x03} 选择了连续递增值，
//     便于检测载荷是否错位或截断
//   - SSRC=0x12345678 选择了每字节不同的值，验证大端序序列化正确
//   - Timestamp=5400000 = 90kHz × 60秒，验证 32 位时间戳的完整范围
//   - SequenceNumber=100 使用较小的值，避免溢出干扰
//
// Google Test 说明：
//   ASSERT_TRUE 用于致命断言——如果 parse 失败，后续检查无意义，
//   应立即终止该测试用例
// ----------------------------------------------------------------------------
TEST(RtpPacketTest, SerializeAndParse) {
    crystal::RtpPacket original;
    original.setMarker(true);
    original.setPayloadType(96);
    original.setSequenceNumber(100);
    // 5400000 = 90kHz × 60秒，验证时间戳在大值下的正确性
    original.setTimestamp(5400000);
    // SSRC 每字节不同，验证大端序编码正确性
    original.setSsrc(0x12345678);
    // 连续递增的载荷数据，便于检测错位或截断
    original.setPayload({0x00, 0x01, 0x02, 0x03});

    // 序列化为字节流（大端序，符合 RFC 3550 线路格式）
    auto data = original.serialize();

    crystal::RtpPacket parsed;
    // 致命断言：如果解析失败，后续所有字段检查都无意义
    ASSERT_TRUE(parsed.parse(data.data(), data.size()));

    // 验证版本号在往返后仍为 2
    EXPECT_EQ(parsed.version(), 2);
    // 验证 marker 位在序列化/反序列化后保持一致
    EXPECT_TRUE(parsed.marker());
    // 验证 payload type 往返一致
    EXPECT_EQ(parsed.payloadType(), 96);
    // 验证序列号往返一致
    EXPECT_EQ(parsed.sequenceNumber(), 100);
    // 验证时间戳往返一致
    EXPECT_EQ(parsed.timestamp(), 5400000u);
    // 验证 SSRC 往返一致
    EXPECT_EQ(parsed.ssrc(), 0x12345678u);
    // 验证载荷长度往返一致
    EXPECT_EQ(parsed.payload().size(), 4u);
    // 验证载荷首字节内容
    EXPECT_EQ(parsed.payload()[0], 0x00);
    // 验证载荷末字节内容（首尾检查确保无截断或错位）
    EXPECT_EQ(parsed.payload()[3], 0x03);
}

// ----------------------------------------------------------------------------
// 测试：RTP 固定头部大小为 12 字节
// ----------------------------------------------------------------------------
// 目的：验证 RTP 固定头部大小符合 RFC 3550 的规定。
//
// 协议行为：
//   RFC 3550 第 5.1 节规定 RTP 固定头部为 12 字节，结构如下：
//   字节 0: V(2bit) + P(1bit) + X(1bit) + CC(4bit)
//   字节 1: M(1bit) + PT(7bit)
//   字节 2-3: Sequence Number (16bit)
//   字节 4-7: Timestamp (32bit)
//   字节 8-11: SSRC (32bit)
//   之后是可选的 CSRC 列表（每个 4 字节，数量由 CC 字段指定）
//
// 测试数据设计思路：
//   - 使用默认构造的 RtpPacket，此时 CC=0，无 CSRC 列表
//   - 因此头部大小恰好等于固定头部的 12 字节
// ----------------------------------------------------------------------------
TEST(RtpPacketTest, HeaderSizeIs12Bytes) {
    crystal::RtpPacket pkt;
    // RFC 3550 规定 RTP 固定头部为 12 字节（V+P+X+CC+M+PT+Seq+TS+SSRC）
    EXPECT_EQ(pkt.headerSize(), 12u);
}

// ----------------------------------------------------------------------------
// 测试：RTP 包总大小 = 头部大小 + 载荷大小
// ----------------------------------------------------------------------------
// 目的：验证 totalSize() 返回值等于 headerSize() + payload.size()。
//
// 协议行为：
//   RTP 包在网络上的总长度 = 固定头部(12) + CSRC(可选) + 扩展(可选) + 载荷 + 填充(可选)
//   本实现中，默认无 CSRC 和扩展，因此 totalSize = 12 + payload.size()
//
// 测试数据设计思路：
//   - 载荷大小选择 5 字节，与 12 字节头部相加得 17
//   - 5 不是 2 的幂次，避免巧合性正确（如果实现误将大小对齐到 4 字节边界，
//     5 字节载荷会暴露这个错误）
// ----------------------------------------------------------------------------
TEST(RtpPacketTest, TotalSizeIsHeaderPlusPayload) {
    crystal::RtpPacket pkt;
    // 载荷大小为 5 字节（非 2 的幂次，避免对齐巧合）
    pkt.setPayload({1, 2, 3, 4, 5});
    // 12 字节头部 + 5 字节载荷 = 17 字节
    EXPECT_EQ(pkt.totalSize(), 17u);
}
