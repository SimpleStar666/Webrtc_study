// ============================================================================
// transport_feedback.h - TWCC TransportFeedback 反馈包（工程化升级 v2 新增）
// ============================================================================
// 【在 GCC 中的角色】
// 接收端把"每个 RTP 包的到达时刻"打包回传给发送端，发送端据此计算
// 单向延迟的变化趋势（差分消时钟偏移——不需要收发时钟同步）。
//
//【报文格式（draft-holmer-rmcat-feedback，libwebrtc 在用）】
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P| FMT=15  |   PT=205     |          length               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                     SSRC of packet sender                     |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                     SSRC of media source                      |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |      base sequence number     | packet status count  |  B=0  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |            reference time (24bit, 1/64ms) |fb pkt cnt| chunks..
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// 之后：N*2B StatusVectorChunk（块）+ M*2B 有符号 delta（250us 单位）
//
//【字段语义】
// - base sequence number：本窗口第一个包的传输层序号（回绕安全）
// - packet status count：窗口覆盖的包数（含丢失的；15 位 + B 位凑 16bit）
// - reference time：基准到达时刻（24 位有符号网格 1/64ms；delta 从它累加）
// - fb pkt count：feedback 包计数（丢 feedback 检测用；本实现恒 0）
//
//【本实现的简化】
// chunk 只用 StatusVectorChunk（2-bit symbol，7 symbol/chunk）：
//   00 = 未收到，10 = 已收到（跟随 2 字节有符号 delta）
// （draft 里 01 是 1 字节 small delta——解码兼容之，编码不用）
// delta 全用 2 字节有符号（250us 单位），即 draft 的"large delta"模式。
// 生产实现还有 RunLengthChunk 压缩连续相同状态——见指南"生产差异"。
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace crystal {

constexpr uint8_t RTCP_FMT_TWCC = 15;  // PT=205 的 TWCC FMT

// 到达样本：seq 与到达时刻（毫秒，接收端本地时钟）
struct ArrivalSample {
    uint16_t seq = 0;
    double arrivalMs = 0;
};

// TWCC 反馈包（编码前/解码后的结构化形态）
struct TwccFeedback {
    uint32_t senderSsrc = 0;   // 发 feedback 一方（接收端）
    uint32_t mediaSsrc = 0;    // 被观测的媒体流（发送端）
    uint16_t baseSeq = 0;     // 本窗口第一个包的传输层序号
    double refTimeMs = 0;     // 基准到达时刻（编码为 1/64ms 网格）
    std::vector<ArrivalSample> received;  // 收到的包（按 seq 升序）
    std::vector<uint16_t> lost;           // 丢失的包（按 seq 升序）
};

// 编码为完整 RTCP 子包字节流（含公共头，可单独作为复合包发送）
std::vector<uint8_t> appendTransportFeedback(const TwccFeedback& fb);

// 解码 feedback 主体（rtcp_packet.cpp 复合包解析 FMT=15 分支调用）
// data 指向 baseSeq 字段（双 SSRC 之后），len 为自该处起的剩余字节数
bool parseTransportFeedback(const uint8_t* data, size_t len, TwccFeedback& fb);

} // namespace crystal
