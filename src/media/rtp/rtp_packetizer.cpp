// ============================================================================
// rtp_packetizer.cpp - RTP 打包器实现
// ============================================================================
//
// 本文件实现了 RtpPacketizer 类的核心打包逻辑，包括 H.264 的 FU-A 分片
// 和 Opus 的直接封装。
//
// 【关键算法 - FU-A 分片】
// FU-A 分片是 H.264 over RTP 中最核心的算法，其核心思想是：
//
// 1. 保留原始 NAL Header 中的 nal_ref_idc（2位）和 type（5位）信息
// 2. 用 FU Indicator 替换原始 NAL Header，其中 type=28 表示这是 FU-A
// 3. 用 FU Header 携带原始 NAL 的 type，以及 Start/End 标记
// 4. 原始 NAL 的数据体（去掉 NAL Header 后的部分）被等分为多个分片
//
// 【分片大小计算】
// 每个 FU-A RTP 包的负载 = FU Indicator(1字节) + FU Header(1字节) + 分片数据
// 因此每个分片的最大数据长度 = maxPacketSize - 2
// ============================================================================

#include "media/rtp/rtp_packetizer.h"
#include "utils/logger.h"

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
//
// 初始化打包器的各项参数。序列号从 startSeqNum 开始，每次打包递增。
// 注意：RFC 3550 建议初始序列号随机选择，以提高安全性（防止已知明文攻击）。
RtpPacketizer::RtpPacketizer(uint8_t payloadType, uint32_t clockRate,
                             uint32_t ssrc, uint16_t startSeqNum,
                             size_t maxPacketSize)
    : payloadType_(payloadType), clockRate_(clockRate), ssrc_(ssrc),
      sequenceNumber_(startSeqNum), maxPacketSize_(maxPacketSize) {}

// ============================================================================
// packetizeH264 - H.264 NAL 单元打包
// ============================================================================
//
// 【算法流程】
//
// 输入: 一个完整的 H.264 NAL 单元（含 NAL Header 字节）
//
// 情况1: NAL 大小 <= maxPacketSize（单 NAL 单元模式）
//   直接将整个 NAL 作为 RTP 负载，标记位 M=1
//
// 情况2: NAL 大小 > maxPacketSize（FU-A 分片模式）
//   步骤:
//   a) 从 NAL Header 提取 nal_ref_idc 和 nal_type
//   b) 去掉原始 NAL Header，得到 NAL 数据体
//   c) 计算每个分片的最大数据长度 = maxPacketSize - 2（减去 FU Indicator + FU Header）
//   d) 循环切分 NAL 数据体，为每个分片构建 FU-A 负载
//   e) 第一个分片: S=1, E=0
//      中间分片:   S=0, E=0
//      最后分片:   S=0, E=1
//   f) 只有最后一个分片的 RTP 标记位 M=1
//
// 【H.264 NAL Header 格式】
//   +---------------+
//   |0|1|2|3|4|5|6|7|
//   +-+-+-+-+-+-+-+-+
//   |F|NRI|  Type   |
//   +---------------+
//   F    (1bit): forbidden_zero_bit，必须为 0
//   NRI  (2bit): nal_ref_idc，指示 NAL 的重要性（0-3，值越大越重要）
//   Type (5bit): NAL 单元类型（1-12 由 H.264 规范定义，28 为 FU-A）
std::vector<RtpPacket> RtpPacketizer::packetizeH264(
    const std::vector<uint8_t>& nalUnit, uint32_t timestamp) {

    std::vector<RtpPacket> result;

    // ========================================================================
    // 情况1: 单 NAL 单元模式
    // NAL 大小不超过最大包大小，直接封装为一个 RTP 包
    // ========================================================================
    if (nalUnit.size() <= maxPacketSize_) {
        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);  // 序列号递增
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(true);  // 单 NAL 模式下，该包就是帧的最后一个包，M=1
        pkt.setPayload(nalUnit);  // 整个 NAL 单元作为负载
        result.push_back(std::move(pkt));
        return result;
    }

    // ========================================================================
    // 情况2: FU-A 分片模式
    // NAL 大小超过最大包大小，需要分片传输
    // ========================================================================

    // 提取原始 NAL Header 中的字段
    // nalUnit[0] 是 NAL Header 字节
    // & 0x60 = 0110 0000，提取 bit5-4 即 nal_ref_idc (NRI)
    // >> 5 将其移到最低 2 位
    uint8_t nalRefIdc = (nalUnit[0] & 0x60) >> 5;

    // & 0x1F = 0001 1111，提取 bit4-0 即 NAL 类型
    uint8_t nalType = nalUnit[0] & 0x1F;

    // NAL 数据体：跳过 NAL Header（1字节），从第 2 字节开始
    const uint8_t* nalData = nalUnit.data() + 1;
    size_t nalDataLen = nalUnit.size() - 1;

    // 每个 FU-A 分片的最大数据长度
    // 减 2 是因为每个分片需要额外 2 字节开销：FU Indicator(1) + FU Header(1)
    size_t maxFragLen = maxPacketSize_ - 2;

    // 循环分片
    size_t offset = 0;
    while (offset < nalDataLen) {
        // 计算当前分片的数据长度
        // 最后一个分片可能不满 maxFragLen
        size_t fragLen = std::min(maxFragLen, nalDataLen - offset);

        // 判断是否为第一个和最后一个分片
        bool isFirst = (offset == 0);
        bool isLast = (offset + fragLen >= nalDataLen);

        // 构建 FU-A 负载
        std::vector<uint8_t> fuPayload;
        fuPayload.reserve(2 + fragLen);

        // ------------------------------------------------------------------
        // FU Indicator 字节（1字节）
        // 格式: [forbidden_zero_bit(1)] [nal_ref_idc(2)] [type=28(5)]
        //
        // nalRefIdc << 5: 将原始 NAL 的 NRI 放到 bit6-5 位置
        // | 28: FU-A 类型编号为 28，放到 bit4-0
        // 例如: nalRefIdc=3, type=28 → (3<<5)|28 = 0x60|0x1C = 0x7C
        // ------------------------------------------------------------------
        uint8_t fuIndicator = (nalRefIdc << 5) | 28;
        fuPayload.push_back(fuIndicator);

        // ------------------------------------------------------------------
        // FU Header 字节（1字节）
        // 格式: [S(1)] [E(1)] [R(1)] [Type(5)]
        //
        // S (Start bit): 第一个分片置 1
        // E (End bit):   最后一个分片置 1
        // R (Reserved):  必须为 0
        // Type:          原始 NAL 的类型
        //
        // 0x80 = 1000 0000，设置 Start bit
        // 0x40 = 0100 0000，设置 End bit
        // ------------------------------------------------------------------
        uint8_t fuHeader = (nalType & 0x1F);
        if (isFirst) fuHeader |= 0x80;  // Start bit
        if (isLast) fuHeader |= 0x40;   // End bit
        fuPayload.push_back(fuHeader);

        // 追加当前分片的 NAL 数据
        fuPayload.insert(fuPayload.end(), nalData + offset,
                         nalData + offset + fragLen);

        // 构建 RTP 包
        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);  // 序列号递增
        pkt.setTimestamp(timestamp);  // 同一帧的所有分片共享相同时间戳
        pkt.setSsrc(ssrc_);
        pkt.setMarker(isLast);  // 只有最后一个分片标记位 M=1
        pkt.setPayload(fuPayload);
        result.push_back(std::move(pkt));

        offset += fragLen;  // 移动到下一个分片
    }

    Logger::debug("FU-A fragmented NAL (type={} size={}) into {} RTP packets",
                  static_cast<int>(nalType), nalUnit.size(), result.size());
    return result;
}

// ============================================================================
// packetizeOpus - Opus 音频帧打包
// ============================================================================
//
// 【算法说明】
// Opus 音频帧通常很小（20ms 帧约 100-400 字节），远小于 MTU 限制，
// 因此不需要分片，直接将整个 Opus 帧封装为一个 RTP 包。
//
// 【Opus 时间戳计算】
// Opus 时钟频率为 48000 Hz，每个帧的时间戳增量 = 帧长度(ms) * 48
// 例如: 20ms 帧 → 时间戳增量 = 20 * 48 = 960
//
// 【标记位】
// RFC 7587 规定 Opus RTP 包的标记位 M=1 表示该包包含音频的最后一个帧
// （当 RTP 包中包含多个 Opus 帧时）。本实现中每个包只包含一个帧，故 M=1。
std::vector<RtpPacket> RtpPacketizer::packetizeOpus(
    const std::vector<uint8_t>& opusFrame, uint32_t timestamp) {

    RtpPacket pkt;
    pkt.setPayloadType(payloadType_);
    pkt.setSequenceNumber(sequenceNumber_++);  // 序列号递增
    pkt.setTimestamp(timestamp);
    pkt.setSsrc(ssrc_);
    pkt.setMarker(true);  // Opus 帧不需要分片，M=1
    pkt.setPayload(opusFrame);

    std::vector<RtpPacket> result;
    result.push_back(std::move(pkt));
    return result;
}

} // namespace crystal
