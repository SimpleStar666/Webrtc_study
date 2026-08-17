// ============================================================================
// rtcp_packet.h - RTCP 报文协议层定义
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// RTCP（RTP Control Protocol，RFC 3550）与 RTP 同端口复用，负责传输质量
// 反馈。本项目自实现以下子集：
//   SR  (PT=200) 发送报告：发送侧的发包计数 + NTP/RTP 时间戳（算 RTT 用）
//   RR  (PT=201) 接收报告：接收侧的丢包率/抖动/最高序列号统计
//   SDES(PT=202) 源描述：仅实现 CNAME（复合包必备）
//   NACK(PT=205, FMT=1, RFC 4585) 传输层反馈：请求重传丢失的 RTP 包
//   PLI (PT=206, FMT=1, RFC 4585) 负载层反馈：请求立即编码关键帧
//
// 【RTP/RTCP 去复用（RFC 5761 Section 4）】
// RTP 与 RTCP 同端口复用，区分依据是第 2 个字节：
//   RTCP 包类型 ∈ [192, 223]；RTP 的 PT 是 7 位（0-127）
// 本项目 PT=96/97（M=1 时第二字节为 224/225），与 RTCP 区间不冲突。
//
// 【NACK 的 PID + BLP 位图（RFC 4585 Section 4.3.1）】
// 一个 FCI 条目 4 字节：PID(16bit) + BLP(16bit)。
// BLP 的第 i 位（0-15）表示序列号 PID+i+1 也丢失，
// 因此一个条目可表达最多 17 个连续丢失包，大幅降低 RTCP 开销。
// ============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace crystal {

// ---- RTCP 包类型常量（第二字节的取值）----
constexpr uint8_t RTCP_PT_SR    = 200;  // Sender Report
constexpr uint8_t RTCP_PT_RR    = 201;  // Receiver Report
constexpr uint8_t RTCP_PT_SDES  = 202;  // Source Description
constexpr uint8_t RTCP_PT_RTPFB = 205;  // 传输层反馈（NACK: FMT=1）
constexpr uint8_t RTCP_PT_PSFB  = 206;  // 负载层反馈（PLI: FMT=1）
constexpr uint8_t RTCP_FMT_NACK = 1;
constexpr uint8_t RTCP_FMT_PLI  = 1;

// RTP/RTCP 去复用判定（RFC 5761：第二字节 ∈ [192,223] 为 RTCP）
bool isRtcpPacket(const uint8_t* data, size_t size);

// ============================================================================
// 数据结构定义
// ============================================================================

// 接收报告块（RFC 3550 Section 6.4.1，24 字节，SR/RR 共用）
struct ReportBlock {
    uint32_t ssrc = 0;            // 被统计源的 SSRC
    uint8_t  fractionLost = 0;    // 自上份报告以来的丢包率（256 = 100%）
    uint32_t cumulativeLost = 0;  // 会话累计丢包数（24 位，最大 0xFFFFFF）
    uint32_t extHighestSeq = 0;   // 扩展最高序列号 = 回绕计数<<16 | 最高序列号
    uint32_t jitter = 0;          // 到达间隔抖动（RTP 时间单位，RFC 3550 A.8）
    uint32_t lsr = 0;             // 最后收到的 SR 的 NTP 时间戳中间 32 位
    uint32_t dlsr = 0;            // 收到上个 SR 至今的延迟（1/65536 秒单位）

    void serialize(std::vector<uint8_t>& out) const;  // 追加 24 字节
};

// 发送报告 SR（PT=200）
struct SenderReport {
    uint32_t ssrc = 0;
    uint64_t ntpTimestamp = 0;    // 64 位 NTP 时间戳（高 32 位秒 + 低 32 位小数）
    uint32_t rtpTimestamp = 0;   // 对应的 RTP 时间戳（NTP↔RTP 换算锚点）
    uint32_t packetCount = 0;    // 会话累计发送的 RTP 包数
    uint32_t octetCount = 0;     // 会话累计发送的负载字节数（不含 RTP 头）
    std::vector<ReportBlock> blocks;
};

// 接收报告 RR（PT=201）
struct ReceiverReport {
    uint32_t ssrc = 0;           // 发出此 RR 一方的 SSRC
    std::vector<ReportBlock> blocks;
};

// SDES（PT=202，仅 CNAME 项）
struct SdesPacket {
    uint32_t ssrc = 0;
    std::string cname;
};

// NACK（PT=205 FMT=1）
struct NackEntry {
    uint16_t pid = 0;  // 丢失包基准序列号
    uint16_t blp = 0;  // 位图：第 i 位表示 PID+i+1 也丢失

    // 相等比较（测试与 round-trip 校验用）
    bool operator==(const NackEntry& o) const {
        return pid == o.pid && blp == o.blp;
    }
};

struct NackPacket {
    uint32_t senderSsrc = 0;  // 发 NACK 一方（本端）的 SSRC
    uint32_t mediaSsrc = 0;   // 被请求重传的媒体流 SSRC（对端）
    std::vector<NackEntry> entries;

    // 丢失 seq 列表 → PID+BLP 条目（排序去重 + 贪心合并，正确处理回绕）
    static std::vector<NackEntry> buildEntries(std::vector<uint16_t> lostSeqs);
    // 条目 → 丢失 seq 列表（round-trip 校验/对端解析用）
    static std::vector<uint16_t> expandEntries(const std::vector<NackEntry>& entries);
};

// PLI（PT=206 FMT=1）
struct PliPacket {
    uint32_t senderSsrc = 0;
    uint32_t mediaSsrc = 0;
};

// 解析结果标签
enum class RtcpKind { Unknown, SenderReport, ReceiverReport, Sdes, Nack, Pli };

// 解析出的单个 RTCP 子包（按 kind 取对应字段）
struct RtcpPacket {
    RtcpKind kind = RtcpKind::Unknown;
    SenderReport sr;      // kind == SenderReport 时有效
    ReceiverReport rr;    // kind == ReceiverReport 时有效
    SdesPacket sdes;      // kind == Sdes 时有效
    NackPacket nack;      // kind == Nack 时有效
    PliPacket pli;        // kind == Pli 时有效
};

// 复合包解析：一个 UDP 载荷可含多个 RTCP 子包，逐个回调。
// 畸形子包（版本错/长度越界）即停止解析。
// 返回值：是否完整消费了全部字节。
using RtcpPacketHandler = std::function<void(const RtcpPacket&)>;
bool parseRtcpCompound(const uint8_t* data, size_t size,
                       const RtcpPacketHandler& handler);

// ---- 构造器（追加到复合包缓冲）----
void appendSenderReport(std::vector<uint8_t>& out, const SenderReport& sr);
void appendReceiverReport(std::vector<uint8_t>& out, const ReceiverReport& rr);
void appendSdes(std::vector<uint8_t>& out, const SdesPacket& sdes);
void appendNack(std::vector<uint8_t>& out, const NackPacket& nack);
void appendPli(std::vector<uint8_t>& out, const PliPacket& pli);

// ---- NTP 时间工具 ----
// 当前系统时间的 64 位 NTP 时间戳（1900 纪元）
uint64_t nowNtp();
// NTP 中间 32 位（SR 的 LSR 字段格式：秒(16b)+小数(16b)）
uint32_t ntpMiddle32(uint64_t ntp);

} // namespace crystal
