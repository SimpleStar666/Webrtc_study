// ============================================================================
// rtp_packetizer.h - RTP 打包器定义
// ============================================================================
//
// 本文件定义了 CrystalRTC 项目中的 RTP 打包器 RtpPacketizer。
//
// 【在 WebRTC 系统中的角色】
// 打包器（Packetizer）负责将编码后的音视频数据分割为适合网络传输的 RTP 包。
// 在发送端，编码器（如 H.264 编码器、Opus 编码器）输出的数据帧可能很大
// （例如一个 H.264 I 帧可达数十 KB），超过了 MTU（Maximum Transmission Unit，
// 通常为 1500 字节减去 IP/UDP 头部 28 字节 ≈ 1472 字节，WebRTC 中通常限制
// RTP 包最大为 1200 字节以确保穿越各种网络设备），因此需要分片传输。
//
// 【核心概念 - H.264 RTP 负载格式（RFC 6184）】
// RFC 6184 定义了 H.264 视频在 RTP 中的传输格式，支持三种模式：
//
// 1. 单 NAL 单元模式（Single NAL Unit Mode）:
//    一个 RTP 包恰好包含一个完整的 NAL 单元，适用于小 NAL（如 SPS/PPS/SEI）
//
// 2. STAP-A（Single-Time Aggregation Packet Type A）:
//    多个 NAL 单元聚合在一个 RTP 包中，适用于多个小 NAL 的场景
//
// 3. FU-A（Fragmentation Unit Type A）:
//    将一个大的 NAL 单元分片为多个 RTP 包传输，是最重要的分片机制
//
// 【FU-A 分片原理】
// 当 NAL 单元大小超过 maxPacketSize 时，使用 FU-A 分片：
//
//   原始 NAL: [NAL Header (1字节)] [NAL Data...]
//
//   FU-A RTP 包负载格式:
//   +---------------+---------------+----------------------------+
//   | FU Indicator  |  FU Header    |  FU Payload (分片数据)      |
//   +---------------+---------------+----------------------------+
//
//   FU Indicator (1字节):
//     forbidden_zero_bit (1bit) = 0
//     nal_ref_idc (2bit)       = 原始 NAL 的 nal_ref_idc
//     type (5bit)              = 28 (FU-A 类型)
//
//   FU Header (1字节):
//     S (1bit) = Start bit，第一个分片置 1
//     E (1bit) = End bit，最后一个分片置 1
//     R (1bit) = Reserved，必须为 0
//     Type (5bit) = 原始 NAL 的 type
//
//   只有 Start 包的 S=1，只有 End 包的 E=1，中间包 S=E=0。
//   接收端通过收集同一时间戳的所有 FU-A 分片来重建原始 NAL 单元。
//
// 【Opus RTP 负载格式（RFC 7587）】
// Opus 音频帧通常较小（几十字节到几百字节），不需要分片，每个 Opus 帧
// 直接封装为一个 RTP 包，标记位 M 置 1。
// ============================================================================

#pragma once

#include "media/rtp/rtp_packet.h"
#include <cstdint>
#include <vector>

namespace crystal {

// ============================================================================
// RtpPacketizer 类 - RTP 打包器
// ============================================================================
//
// 【职责】
// 将编码后的音视频数据帧封装为 RTP 包序列。对于 H.264 视频，支持单 NAL
// 单元模式和 FU-A 分片模式；对于 Opus 音频，直接封装为单个 RTP 包。
//
// 【设计思路】
// 打包器维护一个自增的序列号计数器，每次生成 RTP 包时自动递增。
// 同一帧的所有 RTP 包共享相同的时间戳，通过标记位 M 标识帧的最后一个包。
//
// 【与 RFC 规范的对应】
// - H.264 打包: 遵循 RFC 6184 "RTP Payload Format for H.264 Video"
// - Opus 打包:  遵循 RFC 7587 "RTP Payload Format for Opus Speech and Audio Codec"
// ============================================================================
class RtpPacketizer {
public:
    // 构造函数
    // 参数:
    //   payloadType   - 负载类型（PT），通过 SDP 协商确定，通常为 96-127 的动态类型
    //   clockRate     - 时钟频率（Hz），H.264 为 90000，Opus 为 48000
    //   ssrc          - 同步源标识符，标识本端 RTP 数据流
    //   startSeqNum   - 初始序列号，默认为 0（实际应用中应随机初始化以防攻击）
    //   maxPacketSize - RTP 包最大负载大小（字节），默认 1200，确保不超过 MTU
    RtpPacketizer(uint8_t payloadType, uint32_t clockRate, uint32_t ssrc,
                  uint16_t startSeqNum = 0, size_t maxPacketSize = 1200);

    // 将 H.264 NAL 单元打包为 RTP 包序列
    // 参数:
    //   nalUnit   - 完整的 H.264 NAL 单元数据（含 NAL Header 字节）
    //   timestamp - 该帧的 RTP 时间戳
    // 返回值:
    //   RTP 包向量，可能包含 1 个包（单 NAL 模式）或多个包（FU-A 分片模式）
    // 核心逻辑:
    //   - NAL 大小 <= maxPacketSize: 单 NAL 单元模式，直接封装
    //   - NAL 大小 > maxPacketSize: FU-A 分片模式，拆分为多个 RTP 包
    std::vector<RtpPacket> packetizeH264(const std::vector<uint8_t>& nalUnit,
                                          uint32_t timestamp);

    // 将 Opus 音频帧打包为 RTP 包
    // 参数:
    //   opusFrame - Opus 编码帧数据
    //   timestamp - 该帧的 RTP 时间戳
    // 返回值:
    //   包含单个 RTP 包的向量（Opus 帧不需要分片）
    std::vector<RtpPacket> packetizeOpus(const std::vector<uint8_t>& opusFrame,
                                          uint32_t timestamp);

private:
    // ========================================================================
    // 成员变量
    // ========================================================================

    // 负载类型（PT），7 位无符号整数
    // 对应 RTP 头部中的 PT 字段，标识编码格式
    uint8_t payloadType_;

    // 时钟频率（Hz）
    // H.264: 90000 Hz（每帧间隔约 90000/fps 个时间戳单位）
    // Opus:  48000 Hz（每帧间隔 = 帧长度(ms) * 48）
    uint32_t clockRate_;

    // 同步源标识符 SSRC
    // 标识本端发送的 RTP 数据流，在会话中应唯一
    uint32_t ssrc_;

    // 当前序列号
    // 每生成一个 RTP 包递增 1，到达 65535 后回绕到 0
    // 注意：序列号回绕是正常行为，接收端需要正确处理
    uint16_t sequenceNumber_;

    // RTP 包最大负载大小（字节）
    // 默认 1200 字节，这是 WebRTC 中常用的安全值
    // 计算依据: MTU(1500) - IP头(20) - UDP头(8) = 1472，留余量取 1200
    size_t maxPacketSize_;
};

} // namespace crystal
