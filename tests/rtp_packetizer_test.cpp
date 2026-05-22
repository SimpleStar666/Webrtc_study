// ============================================================================
// rtp_packetizer_test.cpp — RTP 打包器单元测试
// ============================================================================
// 本测试文件验证 CrystalRTC 中 RtpPacketizer 类的正确性，覆盖以下核心功能：
//
// 1. 小 NAL 单元打包：当 H.264 NAL 单元小于 MTU 时，直接作为单个 RTP 包发送
// 2. 大 NAL 单元 FU-A 分片：当 NAL 单元超过 MTU 时，按 RFC 6184 规定的
//    FU-A (Fragmentation Unit) 模式分片发送
// 3. 序列号递增：验证跨多次 packetize 调用，序列号严格递增 1
// 4. Opus 音频打包：验证音频帧直接封装为单个 RTP 包（音频帧通常远小于 MTU）
//
// 学习要点：
// - RFC 6184 定义了 H.264 的 RTP 载荷格式，支持三种打包模式：
//   Single NAL、Non-Interleaved (STAP-A + FU-A)、Interleaved
// - FU-A 分片是 WebRTC 中 H.264 传输的核心机制，将大 NAL 切成多个 RTP 包
// - Opus 音频帧通常很小（几十到几百字节），无需分片，直接放入 RTP 载荷
// - 默认 MTU 为 1200 字节，这是 WebRTC 的典型值（考虑 ICE/DTLS 封装开销）
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/rtp_packetizer.h"

// ----------------------------------------------------------------------------
// 测试：小 NAL 单元直接打包为单个 RTP 包
// ----------------------------------------------------------------------------
// 目的：验证当 H.264 NAL 单元大小不超过 maxPacketSize（默认 1200 字节）时，
//       packetizeH264 将其直接封装为一个 RTP 包，不进行分片。
//
// 协议行为（RFC 6184 Section 5.2）：
//   当 NAL 单元大小不超过 MTU 时，使用 "Single NAL Unit Packet" 模式，
//   NAL 单元直接作为 RTP 载荷，无需添加额外的分片头部。
//
// 测试数据设计思路：
//   - smallNal = {0x65, 0x88, 0x84, 0x00, 0x40}，仅 5 字节，远小于 1200 字节 MTU
//   - 首字节 0x65 的含义：
//     forbidden_zero_bit=0, nal_ref_idc=11(高优先级), nal_unit_type=5(IDR 切片)
//     选择 IDR 帧是因为这是 H.264 中最重要的帧类型，必须正确传输
//   - timestamp=3000 模拟一个 RTP 时间戳值
//
// Google Test 说明：
//   ASSERT_EQ 用于致命断言——如果包数量不为 1，后续对 packets[0] 的访问
//   会导致越界错误，必须立即终止
// ----------------------------------------------------------------------------
TEST(RtpPacketizerTest, SmallNalPackedAsSingleUnit) {
    // 构造打包器：PT=96（动态视频类型），时钟频率=90kHz（视频标准），SSRC=0x12345678
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    // 5 字节的 IDR 切片 NAL，远小于 1200 字节 MTU，无需分片
    // 0x65: nal_ref_idc=3(高优先级), nal_unit_type=5(IDR 切片)
    std::vector<uint8_t> smallNal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(smallNal, 3000);

    // 致命断言：小 NAL 必须只产生 1 个 RTP 包
    ASSERT_EQ(packets.size(), 1u);
    // 单 NAL 包模式下，marker 位必须为 true（表示这是该帧的最后一个包）
    EXPECT_TRUE(packets[0].marker());
    // 验证 payload type 正确传递
    EXPECT_EQ(packets[0].payloadType(), 96);
    // 验证时间戳正确传递
    EXPECT_EQ(packets[0].timestamp(), 3000u);
    // 验证载荷首字节是原始 NAL 头（Single NAL 模式不修改 NAL 头）
    EXPECT_EQ(packets[0].payload()[0], 0x65);
}

// ----------------------------------------------------------------------------
// 测试：大 NAL 单元通过 FU-A 分片打包
// ----------------------------------------------------------------------------
// 目的：验证当 H.264 NAL 单元超过 maxPacketSize 时，packetizeH264 按
//       RFC 6184 Section 5.8 的 FU-A 模式正确分片。
//
// 协议行为（RFC 6184 Section 5.8 — FU-A）：
//   FU-A 分片将一个 NAL 单元拆分为多个 RTP 包，每个包的载荷格式为：
//   +---------------+
//   |0|1|2|3|4|5|6|7|
//   +-+-+-+-+-+-+-+-+
//   |F|NRI|  Type   |  <- FU Indicator = (nal_ref_idc << 5) | 28
//   +---------------+
//   |S|E|R|  Type   |  <- FU Header = (Start|End|Reserved|Original NAL Type)
//   +---------------+
//   | 分片数据...     |
//
//   - FU Indicator 的 Type=28 表示这是一个 FU-A 包
//   - FU Header 的 S(Start)=1 表示第一个分片，E(End)=1 表示最后一个分片
//   - 中间分片 S=0, E=0
//   - 只有最后一个分片的 RTP marker 位为 1
//   - 所有分片共享相同的时间戳（属于同一帧）
//
// 测试数据设计思路：
//   - largeNal 大小为 3000 字节，超过 1200 字节 MTU，触发 FU-A 分片
//   - 首字节 0x65 (IDR 切片)，其余填充 0xAB
//   - 3000 字节会被分为 ceil((3000-1)/(1200-2)) ≈ 3 个分片
//   - 选择 3000 字节是因为它能产生多个分片，且不是 MTU 的整数倍，
//     可以测试最后一个分片大小不规则的边界情况
//
// 位运算解释：
//   - payload[0] & 0x1F：提取 NAL 类型（低 5 位），FU-A 时应为 28
//   - payload[1] & 0x80：检查 Start 位（最高位）
//   - payload[1] & 0x40：检查 End 位（次高位）
// ----------------------------------------------------------------------------
TEST(RtpPacketizerTest, LargeNalFragmentedWithFUA) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x12345678);

    // 3000 字节的 NAL，首字节 0x65 (IDR 切片)，其余填充 0xAB
    // 3000 > 1200(MTU)，将触发 FU-A 分片
    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    // 3000 字节的 NAL 应被分为多个 RTP 包
    EXPECT_GT(packets.size(), 1u);

    // ---- 验证第一个分片（Start=1, End=0）----
    // FU Indicator 的 NAL 类型应为 28（FU-A）
    EXPECT_EQ(packets[0].payload()[0] & 0x1F, 28);
    // FU Header 的 Start 位应为 1（0x80 = 最高位）
    EXPECT_TRUE(packets[0].payload()[1] & 0x80);
    // FU Header 的 End 位应为 0
    EXPECT_FALSE(packets[0].payload()[1] & 0x40);
    // 第一个分片的 marker 位应为 false（不是帧的最后一个包）
    EXPECT_FALSE(packets[0].marker());

    // ---- 验证中间分片（Start=0, End=0）----
    for (size_t i = 1; i < packets.size() - 1; i++) {
        // 所有分片的 FU Indicator NAL 类型都应为 28
        EXPECT_EQ(packets[i].payload()[0] & 0x1F, 28);
        // 中间分片 Start 位为 0
        EXPECT_FALSE(packets[i].payload()[1] & 0x80);
        // 中间分片 End 位为 0
        EXPECT_FALSE(packets[i].payload()[1] & 0x40);
        // 中间分片 marker 位为 false
        EXPECT_FALSE(packets[i].marker());
    }

    // ---- 验证最后一个分片（Start=0, End=1）----
    // FU Indicator NAL 类型仍为 28
    EXPECT_EQ(packets.back().payload()[0] & 0x1F, 28);
    // 最后分片 Start 位为 0
    EXPECT_FALSE(packets.back().payload()[1] & 0x80);
    // 最后分片 End 位为 1（0x40 = 次高位）
    EXPECT_TRUE(packets.back().payload()[1] & 0x40);
    // 最后分片 marker 位为 true（帧结束标记）
    EXPECT_TRUE(packets.back().marker());

    // ---- 验证所有分片共享相同的时间戳和 SSRC ----
    // RFC 3550 规定：同一帧的所有 RTP 包必须使用相同的时间戳
    for (const auto& pkt : packets) {
        EXPECT_EQ(pkt.timestamp(), 5000u);
        EXPECT_EQ(pkt.ssrc(), 0x12345678u);
    }
}

// ----------------------------------------------------------------------------
// 测试：序列号跨多次 packetize 调用严格递增
// ----------------------------------------------------------------------------
// 目的：验证 RtpPacketizer 内部维护的序列号计数器在多次 packetizeH264
//       调用之间严格递增 1，不会重置或跳跃。
//
// 协议行为（RFC 3550 Section 5.1）：
//   序列号初始值应随机选择，之后每发送一个 RTP 包递增 1。
//   序列号用于接收端检测丢包和重排序，必须严格单调递增。
//   序列号为 16 位无符号整数，达到 65535 后回绕到 0。
//
// 测试数据设计思路：
//   - 使用两个极小的 NAL（各 2 字节），各产生 1 个 RTP 包
//   - 这样可以精确验证：第二次调用的第一个包的序列号 = 第一次调用
//     最后一个包的序列号 + 1
//   - NAL 首字节 0x65 (IDR 切片)，第二字节分别为 0x01 和 0x02 以示区分
//
// Google Test 说明：
//   static_cast<uint16_t> 用于处理序列号回绕的情况（虽然本测试不会触发）
// ----------------------------------------------------------------------------
TEST(RtpPacketizerTest, SequenceNumbersIncrement) {
    crystal::RtpPacketizer pktizer(96, 90000, 0xABCD);

    // 两个极小的 NAL，各只产生 1 个 RTP 包
    std::vector<uint8_t> nal1 = {0x65, 0x01};
    std::vector<uint8_t> nal2 = {0x65, 0x02};

    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    // 获取第一次调用的最后一个包的序列号
    uint16_t lastSeq = pkts1.back().sequenceNumber();
    // 获取第二次调用的第一个包的序列号
    uint16_t firstSeq = pkts2.front().sequenceNumber();
    // 序列号必须严格递增 1（使用 uint16_t 转换以正确处理 65535→0 的回绕）
    EXPECT_EQ(firstSeq, static_cast<uint16_t>(lastSeq + 1));
}

// ----------------------------------------------------------------------------
// 测试：Opus 音频帧打包为单个 RTP 包
// ----------------------------------------------------------------------------
// 目的：验证 Opus 音频帧通过 packetizeOpus 封装为单个 RTP 包，
//       且 marker 位、payload type、时间戳、载荷内容正确。
//
// 协议行为（RFC 7587 — Opus 的 RTP 载荷格式）：
//   Opus 编码帧通常很小（2.5ms~120ms 对应几十到几千字节），
//   在 WebRTC 中一般不需要分片，直接作为 RTP 载荷发送。
//   每个 Opus 帧对应一个 RTP 包，marker 位通常设为 1
//   （表示音频帧的开始，有助于接收端快速开始播放）。
//
// 测试数据设计思路：
//   - opusFrame 大小为 80 字节，这是 Opus 20ms 帧（48kHz）的典型大小
//   - 填充值 0xCC 无特殊含义，仅作为可辨识的测试数据
//   - PT=97 是动态 payload type，WebRTC 中常用于 Opus 音频
//   - clockRate=48000 是 Opus 的标准采样率
//   - timestamp=960 对应 48kHz 下 20ms 的采样数（48000 × 0.02 = 960）
// ----------------------------------------------------------------------------
TEST(RtpPacketizerTest, AudioPacketizeSinglePacket) {
    // PT=97（动态音频类型），时钟频率=48kHz（Opus 标准采样率），SSRC=0xBEEFCAFE
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEFCAFE);

    // 80 字节的 Opus 帧，模拟 48kHz/20ms 的典型音频帧
    std::vector<uint8_t> opusFrame(80, 0xCC);
    // timestamp=960 = 48000 × 0.02，对应 20ms 的采样数
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    // Opus 帧必须只产生 1 个 RTP 包（音频帧不需要分片）
    ASSERT_EQ(packets.size(), 1u);
    // 音频帧的 marker 位为 true（RFC 7587 建议音频帧开始时设置）
    EXPECT_TRUE(packets[0].marker());
    // 验证音频 payload type 正确传递
    EXPECT_EQ(packets[0].payloadType(), 97);
    // 验证时间戳正确传递（960 = 20ms @ 48kHz）
    EXPECT_EQ(packets[0].timestamp(), 960u);
    // 验证载荷大小与原始 Opus 帧一致
    EXPECT_EQ(packets[0].payload().size(), 80u);
}
