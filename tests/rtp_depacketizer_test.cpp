// ============================================================================
// rtp_depacketizer_test.cpp — RTP 解包器单元测试
// ============================================================================
// 本测试文件验证 CrystalRTC 中 RtpDepacketizer 类的正确性，覆盖以下核心功能：
//
// 1. 单 NAL 单元重建：验证小 NAL 包经打包→解包后能完整还原
// 2. FU-A 分片重组：验证大 NAL 经 FU-A 分片后，解包器能将所有分片
//    正确重组为原始 NAL 单元
// 3. 多 NAL 帧序列：验证连续多个 NAL 帧的打包→解包流程，
//    确保解包器能正确区分不同帧并独立还原
// 4. Opus 音频帧提取：验证 Opus 帧经打包→解包后完整还原
//
// 学习要点：
// - RtpDepacketizer 是 RtpPacketizer 的逆操作，负责从 RTP 包中还原原始媒体数据
// - FU-A 重组是有状态的：解包器需要维护一个缓冲区(fuBuffer_)和状态标志(fuStarted_)
//   来累积分片数据，直到收到 End 位才输出完整的 NAL 单元
// - 本测试采用"打包→解包"的端到端验证方式，使用 RtpPacketizer 生成测试数据，
//   这比手工构造 RTP 包更可靠，也更贴近实际使用场景
// - depacketizeH264() 返回 vector<vector<uint8_t>>，因为一个 RTP 包可能触发
//   输出（如 FU-A 的最后一个分片触发完整 NAL 输出），也可能不输出（中间分片）
// ============================================================================

#include <gtest/gtest.h>
#include "media/rtp/rtp_depacketizer.h"
#include "media/rtp/rtp_packetizer.h"

// ----------------------------------------------------------------------------
// 测试：单 NAL 单元经打包→解包后完整重建
// ----------------------------------------------------------------------------
// 目的：验证小 NAL 单元通过 Single NAL Unit Packet 模式传输后，
//       解包器能正确还原原始 NAL 数据，不丢失或篡改任何字节。
//
// 协议行为（RFC 6184 Section 5.2）：
//   Single NAL Unit Packet 的载荷就是完整的 NAL 单元，
//   解包器只需直接提取载荷即可，无需任何重组操作。
//
// 测试数据设计思路：
//   - NAL = {0x65, 0x88, 0x84, 0x00, 0x40}，5 字节的 IDR 切片
//   - 0x65: nal_ref_idc=3, nal_unit_type=5(IDR)，这是 H.264 最关键的帧类型
//   - 使用打包器生成测试数据，确保打包→解包的端到端一致性
//   - 循环遍历所有生成的 RTP 包（本例只有 1 个），验证每个包解包后
//     都能产生与原始 NAL 完全相同的数据
//
// Google Test 说明：
//   ASSERT_EQ 确保解包结果数量正确后再访问元素，避免越界
// ----------------------------------------------------------------------------
TEST(RtpDepacketizerTest, SingleNalReconstructed) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    // 5 字节的 IDR 切片 NAL，打包后只有 1 个 RTP 包
    std::vector<uint8_t> nal = {0x65, 0x88, 0x84, 0x00, 0x40};
    auto packets = pktizer.packetizeH264(nal, 3000);

    // 遍历所有 RTP 包（小 NAL 只有 1 个包）
    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        // 单 NAL 包解包后必须恰好产生 1 个 NAL 单元
        ASSERT_EQ(nals.size(), 1u);
        // 解包后的 NAL 必须与原始 NAL 逐字节相同
        EXPECT_EQ(nals[0], nal);
    }
}

// ----------------------------------------------------------------------------
// 测试：FU-A 分片经解包后正确重组为完整 NAL 单元
// ----------------------------------------------------------------------------
// 目的：验证大 NAL 单元经 FU-A 分片传输后，解包器能将所有分片
//       正确重组为原始 NAL 单元，包括 NAL 头的重建。
//
// 协议行为（RFC 6184 Section 5.8 — FU-A 重组）：
//   解包器收到 FU-A 分片时的处理逻辑：
//   1. 收到 Start 分片：创建新缓冲区，重建 NAL 头
//      （从 FU Indicator 的 NRI 和 FU Header 的原始 NAL Type 组合），
//      然后追加分片数据
//   2. 收到 Middle 分片：将分片数据追加到缓冲区
//   3. 收到 End 分片：追加分片数据，输出完整 NAL 单元，清空缓冲区
//
//   NAL 头重建过程：
//   原始 NAL 头 = (FU Indicator 的 NRI 位) | (FU Header 的原始 NAL Type)
//   例如：FU Indicator=0x65(NRI=11, Type=28), FU Header=0x85(Start, Type=5)
//   重建后：NRI=11, Type=5 → 0x65，与原始 NAL 头完全一致
//
// 测试数据设计思路：
//   - largeNal 大小为 3000 字节，会被分为约 3 个 FU-A 分片
//   - 首字节 0x65 (IDR 切片)，其余填充 0xAB
//   - 逐包送入解包器，中间分片不会产生输出（nals 为空），
//     只有最后一个分片才会输出完整的重组 NAL
//   - 使用 allNals 收集所有解包输出，最终应恰好有 1 个 NAL
//
// Google Test 说明：
//   ASSERT_EQ(allNals.size(), 1u) 确保只产生了 1 个 NAL 后再比较内容
// ----------------------------------------------------------------------------
TEST(RtpDepacketizerTest, FUAFragmentsReassembled) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    // 3000 字节的 NAL，会被 FU-A 分为多个 RTP 包
    std::vector<uint8_t> largeNal(3000, 0xAB);
    largeNal[0] = 0x65;

    auto packets = pktizer.packetizeH264(largeNal, 5000);

    // 收集所有解包输出的 NAL 单元
    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : packets) {
        auto nals = depktizer.depacketizeH264(pkt);
        // 中间分片的 nals 为空，最后一个分片才输出完整 NAL
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    // 所有分片重组后必须恰好产生 1 个 NAL 单元
    ASSERT_EQ(allNals.size(), 1u);
    // 重组后的 NAL 必须与原始 NAL 逐字节相同（包括 NAL 头和所有分片数据）
    EXPECT_EQ(allNals[0], largeNal);
}

// ----------------------------------------------------------------------------
// 测试：多个 NAL 帧序列经打包→解包后独立还原
// ----------------------------------------------------------------------------
// 目的：验证解包器在处理连续多个 NAL 帧时，能正确区分不同帧并独立还原，
//       不会出现帧间数据混淆。
//
// 协议行为：
//   在实际 WebRTC 通话中，RTP 流包含连续的视频帧，每帧可能包含一个或多个 NAL。
//   解包器必须能正确处理帧边界——当收到新帧的 Single NAL 包时，
//   如果当前正在重组 FU-A 分片，应丢弃不完整的缓冲区（参见 depacketizer.cpp
//   中 "Dropping incomplete FU-A buffer on single NAL arrival" 的逻辑）。
//
// 测试数据设计思路：
//   - 两个小 NAL（各 3 字节），分别打包为独立的 RTP 包
//   - 使用不同的时间戳（1000 和 2000）模拟不同的视频帧
//   - NAL 内容不同（0x01/0x02 vs 0x03/0x04），便于验证帧间不混淆
//   - 依次将所有 RTP 包送入同一个解包器，验证最终输出 2 个独立的 NAL
//
// 边界条件：
//   - 此测试隐式验证了：当 Single NAL 到达时，解包器能正确处理
//     （即使之前没有 FU-A 状态需要清理）
// ----------------------------------------------------------------------------
TEST(RtpDepacketizerTest, MultipleNalsProduceMultipleOutputs) {
    crystal::RtpPacketizer pktizer(96, 90000, 0x1234);
    crystal::RtpDepacketizer depktizer;

    // 两个不同的 NAL，模拟两个视频帧
    std::vector<uint8_t> nal1 = {0x65, 0x01, 0x02};
    std::vector<uint8_t> nal2 = {0x65, 0x03, 0x04};

    // 分别打包（不同的时间戳表示不同的帧）
    auto pkts1 = pktizer.packetizeH264(nal1, 1000);
    auto pkts2 = pktizer.packetizeH264(nal2, 2000);

    // 收集所有解包输出
    std::vector<std::vector<uint8_t>> allNals;
    for (const auto& pkt : pkts1) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }
    for (const auto& pkt : pkts2) {
        auto nals = depktizer.depacketizeH264(pkt);
        allNals.insert(allNals.end(), nals.begin(), nals.end());
    }

    // 必须恰好产生 2 个 NAL 单元（每个输入 NAL 对应一个输出）
    ASSERT_EQ(allNals.size(), 2u);
    // 第一个 NAL 必须与原始 nal1 逐字节相同
    EXPECT_EQ(allNals[0], nal1);
    // 第二个 NAL 必须与原始 nal2 逐字节相同（验证帧间不混淆）
    EXPECT_EQ(allNals[1], nal2);
}

// ----------------------------------------------------------------------------
// 测试：Opus 音频帧经打包→解包后完整提取
// ----------------------------------------------------------------------------
// 目的：验证 Opus 音频帧经打包→解包后能完整还原，不丢失或篡改任何字节。
//
// 协议行为（RFC 7587）：
//   Opus 的 RTP 载荷格式非常简单——RTP 载荷就是完整的 Opus 编码帧，
//   解包器只需直接提取载荷即可，无需任何重组或解析操作。
//   这与 H.264 的 FU-A 重组形成鲜明对比——音频帧通常足够小，无需分片。
//
// 测试数据设计思路：
//   - opusFrame 大小为 80 字节，模拟 Opus 20ms 帧（48kHz）
//   - 填充值 0xCC，便于验证数据完整性
//   - PT=97 是 WebRTC 中 Opus 的常用动态 payload type
//   - 先用打包器生成 RTP 包，再用解包器提取，验证端到端一致性
//
// Google Test 说明：
//   两个 ASSERT_EQ 分别确保打包和解包步骤的输出数量正确
// ----------------------------------------------------------------------------
TEST(RtpDepacketizerTest, OpusFrameExtracted) {
    crystal::RtpPacketizer pktizer(97, 48000, 0xBEEF);
    crystal::RtpDepacketizer depktizer;

    // 80 字节的 Opus 帧，模拟 48kHz/20ms 的典型音频帧
    std::vector<uint8_t> opusFrame(80, 0xCC);
    auto packets = pktizer.packetizeOpus(opusFrame, 960);

    // 打包后必须恰好产生 1 个 RTP 包
    ASSERT_EQ(packets.size(), 1u);
    // 解包 Opus 帧
    auto frames = depktizer.depacketizeOpus(packets[0]);
    // 解包后必须恰好产生 1 个音频帧
    ASSERT_EQ(frames.size(), 1u);
    // 解包后的帧必须与原始 Opus 帧逐字节相同
    EXPECT_EQ(frames[0], opusFrame);
}
