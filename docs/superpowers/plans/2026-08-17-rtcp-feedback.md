# RTCP 反馈体系（NACK + PLI + SR/RR）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 CrystalRTC P2P 客户端增加全自实现的 RTCP 反馈体系——NACK 丢包重传、PLI 关键帧请求、SR/RR 质量统计（丢包率/RTT/抖动），弱网下视频质量可观测、可恢复。

**Architecture:** 新增 `src/media/rtcp/` 模块（与 `rtp/` 平级），协议层（rtcp_packet，纯字节无状态）与策略层（retransmission_buffer / nack_requester / rtcp_reporter）分离。libdatachannel 仅作加密字节搬运工：RTP/RTCP 按 RFC 5761 在 `PeerConnection` 内去复用，RTCP 经新增的 `onRtcp`/`sendRtcp` 通道收发。

**Tech Stack:** C++17、CMake、GoogleTest、libdatachannel v0.21.2（已 FetchContent）、FFmpeg/Opus（现有）。

**设计文档:** `docs/superpowers/specs/2026-08-17-rtcp-feedback-design.md`
**勘误（实施时以本计划为准，Task 9 回写设计文档）:**
1. RFC 5761 去复用规则：区分 RTP/RTCP 的是**第 2 个字节**（RTCP 包类型字段 192-223），不是首字节低 7 位。本项目 PT=96/97，M=1 时第二字节为 224/225，不冲突。
2. SSRC 随机化改动位置：`main_client.cpp` 调用侧（`RtpPacketizer` 构造参数已支持 ssrc/startSeqNum，无需改 packetizer 本体）。

**构建/测试命令约定**（每个任务复用）：
```bash
cd /workspace/build && cmake .. > /dev/null && make <目标> -j$(nproc)
./<测试可执行>            # 期望全部 PASS
```
现有测试基线：`./crystal_rtp_tests` 17 用例全绿，任何改动后必须保持。

---

## 文件结构总览

| 文件 | 动作 | 职责 |
|------|------|------|
| `src/media/rtcp/rtcp_packet.h/cpp` | 新建 | 协议层：SR/RR/SDES/NACK/PLI 构造与解析、复合包迭代、NTP 工具、RTP/RTCP 去复用判定 |
| `src/media/rtcp/retransmission_buffer.h/cpp` | 新建 | 发送端：已发视频包环形缓冲，响应 NACK 补发 |
| `src/media/rtcp/nack_requester.h/cpp` | 新建 | 接收端：NACK 请求状态机（立即请求 + 重试 + 放弃） |
| `src/media/rtcp/rtcp_reporter.h/cpp` | 新建 | 统计：SendSideReporter（SR 构造/RTT 计算）、RecvSideReporter（RR 报告块/丢包/抖动） |
| `src/media/rtp/jitter_buffer.h/cpp` | 修改 | `insert()` 返回本次新检测到的丢失序列号 |
| `src/media/video/h264_encoder.h/cpp` | 修改 | 新增 `forceKeyframe()` |
| `src/media/video/h264_decoder.h/cpp` | 修改 | 新增 `onError` 回调 |
| `src/transport/transport_manager.h/cpp` | 修改 | RTP/RTCP 去复用、`onRtcp`/`sendRtcp`、SDP 注入 rtcp-fb |
| `main_client.cpp` | 修改 | 全部接线：随机 SSRC、NACK、PLI、SR/RR 周期收发、统计打印、RTP 时间戳修复 |
| `src/CMakeLists.txt` | 修改 | 新增 `crystal_media_rtcp` 库 |
| `tests/CMakeLists.txt` | 修改 | 新增 `crystal_rtcp_tests` 目标 |
| `tests/rtcp_packet_test.cpp` 等 5 个 | 新建 | 单元测试 |
| `docs/LEARNING_GUIDE.md` | 修改 | 新增 Module 8 |
| `docs/USAGE.md` | 修改 | 统计日志说明、tc netem 弱网测试 |
| `README.md` | 修改 | 特性表 |
| 设计文档 | 修改 | 两处勘误 |

---

### Task 1: RTCP 协议层 `rtcp_packet`

**Files:**
- Create: `src/media/rtcp/rtcp_packet.h`
- Create: `src/media/rtcp/rtcp_packet.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tests/rtcp_packet_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写头文件 `src/media/rtcp/rtcp_packet.h`**

```cpp
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
```

- [ ] **Step 2: 写实现 `src/media/rtcp/rtcp_packet.cpp`**

```cpp
// ============================================================================
// rtcp_packet.cpp - RTCP 报文协议层实现
// ============================================================================
//
// 【字节序】RTCP 所有多字节字段均为网络字节序（大端）。
//
// 【公共头部（4 字节）】
//   字节0: V(2bit)=2 | P(1bit)=0 | RC/FMT(5bit)
//   字节1: 包类型 PT（SR=200/RR=201/SDES=202/RTPFB=205/PSFB=206）
//   字节2-3: length = 整个子包字节数/4 - 1（含头部，不含填充）
//
// 【各子包长度公式】
//   SR:  28 + 24*n（n = 报告块数）
//   RR:  8  + 24*n
//   SDES: 8 + chunk（4 字节对齐，null 填充）
//   NACK: 12 + 4*n（n = FCI 条目数）
//   PLI:  12（无 FCI）
// ============================================================================

#include "media/rtcp/rtcp_packet.h"
#include <algorithm>
#include <chrono>

namespace crystal {

// ============================================================================
// 内部字节序辅助
// ============================================================================
namespace {

void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void putU24(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t getU16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t getU32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

// 解析 24 字节报告块
bool parseReportBlock(const uint8_t* p, ReportBlock& blk) {
    blk.ssrc = getU32(p);
    blk.fractionLost = p[4];
    blk.cumulativeLost = (static_cast<uint32_t>(p[5]) << 16) |
                         (static_cast<uint32_t>(p[6]) << 8) |
                         static_cast<uint32_t>(p[7]);
    blk.extHighestSeq = getU32(p + 8);
    blk.jitter = getU32(p + 12);
    blk.lsr = getU32(p + 16);
    blk.dlsr = getU32(p + 20);
    return true;
}

} // namespace

// ============================================================================
// RTP/RTCP 去复用（RFC 5761）
// ============================================================================
bool isRtcpPacket(const uint8_t* data, size_t size) {
    // 第二字节：RTCP 包类型 192-223；RTP 为 M+PT（本项目 PT=96/97，
    // M=1 时第二字节 224/225，M=0 时 96/97，均落在区间外，判定无歧义）
    return size >= 2 && data[1] >= 192 && data[1] <= 223;
}

// ============================================================================
// 报告块序列化（SR/RR 共用）
// ============================================================================
void ReportBlock::serialize(std::vector<uint8_t>& out) const {
    putU32(out, ssrc);
    out.push_back(fractionLost);
    putU24(out, cumulativeLost & 0xFFFFFF);
    putU32(out, extHighestSeq);
    putU32(out, jitter);
    putU32(out, lsr);
    putU32(out, dlsr);
}

// ============================================================================
// 构造器
// ============================================================================
void appendSenderReport(std::vector<uint8_t>& out, const SenderReport& sr) {
    // 先构造头部之后的 body，便于计算 length 字段
    std::vector<uint8_t> body;
    putU32(body, sr.ssrc);
    putU32(body, static_cast<uint32_t>(sr.ntpTimestamp >> 32));
    putU32(body, static_cast<uint32_t>(sr.ntpTimestamp & 0xFFFFFFFF));
    putU32(body, sr.rtpTimestamp);
    putU32(body, sr.packetCount);
    putU32(body, sr.octetCount);
    for (const auto& blk : sr.blocks) blk.serialize(body);

    out.push_back(static_cast<uint8_t>(0x80 | (sr.blocks.size() & 0x1F)));
    out.push_back(RTCP_PT_SR);
    putU16(out, static_cast<uint16_t>(body.size() / 4));  // = (4+body)/4 - 1
    out.insert(out.end(), body.begin(), body.end());
}

void appendReceiverReport(std::vector<uint8_t>& out, const ReceiverReport& rr) {
    std::vector<uint8_t> body;
    putU32(body, rr.ssrc);
    for (const auto& blk : rr.blocks) blk.serialize(body);

    out.push_back(static_cast<uint8_t>(0x80 | (rr.blocks.size() & 0x1F)));
    out.push_back(RTCP_PT_RR);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendSdes(std::vector<uint8_t>& out, const SdesPacket& sdes) {
    std::vector<uint8_t> body;
    putU32(body, sdes.ssrc);
    body.push_back(1);  // 类型 1 = CNAME
    body.push_back(static_cast<uint8_t>(sdes.cname.size()));
    body.insert(body.end(), sdes.cname.begin(), sdes.cname.end());
    // chunk 以 null 填充至 32 位对齐（RFC 3550 6.5）
    while (body.size() % 4 != 0) body.push_back(0);

    out.push_back(0x80 | 0x01);  // 1 个 chunk
    out.push_back(RTCP_PT_SDES);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendNack(std::vector<uint8_t>& out, const NackPacket& nack) {
    std::vector<uint8_t> body;
    putU32(body, nack.senderSsrc);
    putU32(body, nack.mediaSsrc);
    for (const auto& e : nack.entries) {
        putU16(body, e.pid);
        putU16(body, e.blp);
    }

    out.push_back(0x80 | RTCP_FMT_NACK);  // FMT=1
    out.push_back(RTCP_PT_RTPFB);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendPli(std::vector<uint8_t>& out, const PliPacket& pli) {
    std::vector<uint8_t> body;
    putU32(body, pli.senderSsrc);
    putU32(body, pli.mediaSsrc);

    out.push_back(0x80 | RTCP_FMT_PLI);  // FMT=1
    out.push_back(RTCP_PT_PSFB);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

// ============================================================================
// NACK PID+BLP 合并与展开
// ============================================================================
std::vector<NackEntry> NackPacket::buildEntries(std::vector<uint16_t> lostSeqs) {
    std::sort(lostSeqs.begin(), lostSeqs.end());
    lostSeqs.erase(std::unique(lostSeqs.begin(), lostSeqs.end()), lostSeqs.end());

    std::vector<NackEntry> out;
    for (uint16_t s : lostSeqs) {
        if (out.empty() || static_cast<uint16_t>(s - out.back().pid) > 16) {
            // 距上一个 PID 超过 16 → 无法用位图覆盖，开新条目
            out.push_back({s, 0});
        } else {
            // BLP 第 (s-pid-1) 位置 1，表示 PID+i+1 丢失
            out.back().blp |= static_cast<uint16_t>(1u << (s - out.back().pid - 1));
        }
    }
    return out;
}

std::vector<uint16_t> NackPacket::expandEntries(const std::vector<NackEntry>& entries) {
    std::vector<uint16_t> out;
    for (const auto& e : entries) {
        out.push_back(e.pid);
        for (int i = 0; i < 16; i++) {
            if (e.blp & (1u << i)) out.push_back(static_cast<uint16_t>(e.pid + i + 1));
        }
    }
    return out;
}

// ============================================================================
// 复合包解析
// ============================================================================
bool parseRtcpCompound(const uint8_t* data, size_t size,
                       const RtcpPacketHandler& handler) {
    size_t offset = 0;
    while (offset + 4 <= size) {
        const uint8_t* p = data + offset;

        // 版本号必须为 2
        if ((p[0] >> 6) != 2) return false;

        uint8_t fmtOrCount = p[0] & 0x1F;
        uint8_t pt = p[1];
        size_t byteLen = (static_cast<size_t>(getU16(p + 2)) + 1) * 4;

        // length 字段声明的长度超过剩余数据 → 畸形，停止
        if (offset + byteLen > size) return false;

        size_t bodyLen = byteLen - 4;
        const uint8_t* body = p + 4;

        RtcpPacket pkt;
        switch (pt) {
        case RTCP_PT_SR: {
            if (bodyLen < 24) return false;
            pkt.kind = RtcpKind::SenderReport;
            pkt.sr.ssrc = getU32(body);
            pkt.sr.ntpTimestamp =
                (static_cast<uint64_t>(getU32(body + 4)) << 32) | getU32(body + 8);
            pkt.sr.rtpTimestamp = getU32(body + 12);
            pkt.sr.packetCount = getU32(body + 16);
            pkt.sr.octetCount = getU32(body + 20);
            // 报告块数取 RC 与实际剩余空间的最小值（防御畸形包）
            size_t n = std::min<size_t>(fmtOrCount, (bodyLen - 24) / 24);
            for (size_t i = 0; i < n; i++) {
                ReportBlock blk;
                parseReportBlock(body + 24 + i * 24, blk);
                pkt.sr.blocks.push_back(blk);
            }
            break;
        }
        case RTCP_PT_RR: {
            if (bodyLen < 4) return false;
            pkt.kind = RtcpKind::ReceiverReport;
            pkt.rr.ssrc = getU32(body);
            size_t n = std::min<size_t>(fmtOrCount, (bodyLen - 4) / 24);
            for (size_t i = 0; i < n; i++) {
                ReportBlock blk;
                parseReportBlock(body + 4 + i * 24, blk);
                pkt.rr.blocks.push_back(blk);
            }
            break;
        }
        case RTCP_PT_SDES: {
            if (bodyLen < 4) return false;
            pkt.kind = RtcpKind::Sdes;
            pkt.sdes.ssrc = getU32(body);
            // 仅解析第一个 chunk 的 CNAME 项（本项目每包一个 chunk）
            size_t pos = 4;
            while (pos + 2 <= bodyLen) {
                uint8_t type = body[pos];
                if (type == 0) break;  // chunk 结束标志
                uint8_t len = body[pos + 1];
                if (pos + 2 + len > bodyLen) break;
                if (type == 1) {
                    pkt.sdes.cname.assign(reinterpret_cast<const char*>(body + pos + 2), len);
                }
                pos += 2 + len;
            }
            break;
        }
        case RTCP_PT_RTPFB: {
            if (fmtOrCount == RTCP_FMT_NACK) {  // 仅识别 NACK
                if (bodyLen < 8) return false;
                pkt.kind = RtcpKind::Nack;
                pkt.nack.senderSsrc = getU32(body);
                pkt.nack.mediaSsrc = getU32(body + 4);
                size_t n = (bodyLen - 8) / 4;
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* f = body + 8 + i * 4;
                    pkt.nack.entries.push_back({getU16(f), getU16(f + 2)});
                }
            }
            break;  // 其他 FMT 不识别，跳过该子包
        }
        case RTCP_PT_PSFB: {
            if (fmtOrCount == RTCP_FMT_PLI) {  // 仅识别 PLI
                if (bodyLen < 8) return false;
                pkt.kind = RtcpKind::Pli;
                pkt.pli.senderSsrc = getU32(body);
                pkt.pli.mediaSsrc = getU32(body + 4);
            }
            break;
        }
        default:
            break;  // BYE(203)/APP(204)/未知类型：跳过
        }

        if (pkt.kind != RtcpKind::Unknown && handler) handler(pkt);
        offset += byteLen;
    }
    return offset == size;  // 完整消费才算成功
}

// ============================================================================
// NTP 时间工具
// ============================================================================
uint64_t nowNtp() {
    using namespace std::chrono;
    auto now = system_clock::now().time_since_epoch();
    auto secs = duration_cast<seconds>(now).count();
    auto nanos = duration_cast<nanoseconds>(now).count() % 1000000000LL;
    // NTP 纪元是 1900，Unix 纪元是 1970，偏移 2208988800 秒
    uint64_t secPart = static_cast<uint64_t>(secs) + 2208988800ULL;
    // 纳秒 → 32 位 NTP 小数部分
    uint64_t fracPart =
        static_cast<uint64_t>(nanos) * (1ULL << 32) / 1000000000ULL;
    return (secPart << 32) | fracPart;
}

uint32_t ntpMiddle32(uint64_t ntp) {
    return static_cast<uint32_t>((ntp >> 16) & 0xFFFFFFFF);
}

} // namespace crystal
```

- [ ] **Step 3: 修改 `src/CMakeLists.txt`**，在 `crystal_media_rtp` 段之后追加：

```cmake
# --- Media: RTCP ---
add_library(crystal_media_rtcp STATIC
    media/rtcp/rtcp_packet.cpp
)
target_include_directories(crystal_media_rtcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_rtcp PUBLIC crystal_utils)
```

- [ ] **Step 4: 写测试 `tests/rtcp_packet_test.cpp`**

```cpp
// ============================================================================
// rtcp_packet_test.cpp — RTCP 协议层单元测试
// ============================================================================
// 覆盖：去复用规则、SR/RR/NACK/PLI/SDES round-trip、PID+BLP 位图、
//       复合包迭代、畸形包安全拒绝。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/rtcp_packet.h"

using namespace crystal;

// 毫秒 → NTP（测试辅助，避免浮点误差）
static uint64_t msToNtp(uint64_t ms) {
    return (static_cast<uint64_t>(ms) << 32) / 1000;
}

// ----------------------------------------------------------------------------
// 去复用规则（RFC 5761：第二字节 ∈ [192,223] 为 RTCP）
// ----------------------------------------------------------------------------
TEST(RtcpDemuxRule, DistinguishesRtpAndRtcp) {
    // RTP：V=2 PT=96（byte1=0x60=96）→ 非 RTCP
    uint8_t rtp[] = {0x80, 0x60, 0x00, 0x01};
    EXPECT_FALSE(isRtcpPacket(rtp, sizeof(rtp)));
    // RTP：M=1 PT=96（byte1=224）→ 非 RTCP
    uint8_t rtpMarked[] = {0x80, 0xE0, 0x00, 0x01};
    EXPECT_FALSE(isRtcpPacket(rtpMarked, sizeof(rtpMarked)));
    // RTCP：RR（byte1=201）
    uint8_t rr[] = {0x81, 0xC9, 0x00, 0x01};
    EXPECT_TRUE(isRtcpPacket(rr, sizeof(rr)));
    // RTCP：RTPFB NACK（byte1=205）
    uint8_t nack[] = {0x81, 0xCD, 0x00, 0x02};
    EXPECT_TRUE(isRtcpPacket(nack, sizeof(nack)));
}

TEST(RtcpDemuxRule, BoundaryValues) {
    EXPECT_TRUE(isRtcpPacket((const uint8_t*)"\x80\xc0", 2));  // 192 → RTCP
    EXPECT_TRUE(isRtcpPacket((const uint8_t*)"\x80\xdf", 2));  // 223 → RTCP
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80\xbf", 2)); // 191 → 非
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80\xe0", 2)); // 224 → 非
    EXPECT_FALSE(isRtcpPacket((const uint8_t*)"\x80", 1));     // 太短 → 非
}

// ----------------------------------------------------------------------------
// SR round-trip
// ----------------------------------------------------------------------------
TEST(SenderReportTest, SerializeParseRoundTrip) {
    SenderReport sr;
    sr.ssrc = 0x12345678;
    sr.ntpTimestamp = msToNtp(123456);
    sr.rtpTimestamp = 90000;
    sr.packetCount = 42;
    sr.octetCount = 123456;
    ReportBlock blk;
    blk.ssrc = 0x87654321;
    blk.fractionLost = 25;
    blk.cumulativeLost = 3;
    blk.extHighestSeq = 0x0001FFFF;
    blk.jitter = 7;
    blk.lsr = 0x11223344;
    blk.dlsr = 65536;
    sr.blocks.push_back(blk);

    std::vector<uint8_t> buf;
    appendSenderReport(buf, sr);
    EXPECT_EQ(buf.size(), 28u + 24u);  // SR 头 28 字节 + 1 个报告块

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::SenderReport);
    EXPECT_EQ(got.sr.ssrc, sr.ssrc);
    EXPECT_EQ(got.sr.ntpTimestamp, sr.ntpTimestamp);
    EXPECT_EQ(got.sr.rtpTimestamp, sr.rtpTimestamp);
    EXPECT_EQ(got.sr.packetCount, sr.packetCount);
    EXPECT_EQ(got.sr.octetCount, sr.octetCount);
    ASSERT_EQ(got.sr.blocks.size(), 1u);
    EXPECT_EQ(got.sr.blocks[0].ssrc, blk.ssrc);
    EXPECT_EQ(got.sr.blocks[0].fractionLost, blk.fractionLost);
    EXPECT_EQ(got.sr.blocks[0].cumulativeLost, blk.cumulativeLost);
    EXPECT_EQ(got.sr.blocks[0].extHighestSeq, blk.extHighestSeq);
    EXPECT_EQ(got.sr.blocks[0].jitter, blk.jitter);
    EXPECT_EQ(got.sr.blocks[0].lsr, blk.lsr);
    EXPECT_EQ(got.sr.blocks[0].dlsr, blk.dlsr);
}

// ----------------------------------------------------------------------------
// RR round-trip
// ----------------------------------------------------------------------------
TEST(ReceiverReportTest, SerializeParseRoundTrip) {
    ReceiverReport rr;
    rr.ssrc = 0xAAAA0001;
    ReportBlock blk;
    blk.ssrc = 0xBBBB0002;
    blk.fractionLost = 128;
    blk.cumulativeLost = 100;
    blk.extHighestSeq = 65600;  // 含一次回绕
    rr.blocks.push_back(blk);

    std::vector<uint8_t> buf;
    appendReceiverReport(buf, rr);
    EXPECT_EQ(buf.size(), 8u + 24u);

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::ReceiverReport);
    EXPECT_EQ(got.rr.ssrc, rr.ssrc);
    ASSERT_EQ(got.rr.blocks.size(), 1u);
    EXPECT_EQ(got.rr.blocks[0].fractionLost, 128u);
    EXPECT_EQ(got.rr.blocks[0].extHighestSeq, 65600u);
}

// ----------------------------------------------------------------------------
// NACK：PID+BLP 位图
// ----------------------------------------------------------------------------
TEST(NackBitmapTest, BuildAndExpandRoundTrip) {
    // 连续丢失 100..110（11 个包）+ 孤立的 200
    std::vector<uint16_t> lost = {100, 101, 102, 103, 104, 105,
                                  106, 107, 108, 109, 110, 200};
    auto entries = NackPacket::buildEntries(lost);
    // 100..110 可被一个条目（PID=100, BLP 覆盖 101..110=10 个位）覆盖，
    // 200 超出 100+16 范围 → 新条目
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].pid, 100);
    EXPECT_EQ(entries[0].blp, 0x03FF);  // 位 0..9 全 1
    EXPECT_EQ(entries[1].pid, 200);
    EXPECT_EQ(entries[1].blp, 0);

    auto expanded = NackPacket::expandEntries(entries);
    EXPECT_EQ(expanded, lost);  // 展开后与输入一致（已排序去重）
}

TEST(NackBitmapTest, HandlesWraparound) {
    // 回绕场景：65534, 65535, 0, 1 连续丢失
    std::vector<uint16_t> lost = {65534, 65535, 0, 1};
    auto entries = NackPacket::buildEntries(lost);
    ASSERT_EQ(entries.size(), 1u);  // uint16 空间内仍然连续
    EXPECT_EQ(entries[0].pid, 65534);
    auto expanded = NackPacket::expandEntries(entries);
    EXPECT_EQ(expanded, (std::vector<uint16_t>{65534, 65535, 0, 1}));
}

TEST(NackPacketTest, SerializeParseRoundTrip) {
    NackPacket nack;
    nack.senderSsrc = 0x11111111;
    nack.mediaSsrc = 0x22222222;
    nack.entries = NackPacket::buildEntries({500, 501, 502});

    std::vector<uint8_t> buf;
    appendNack(buf, nack);
    EXPECT_EQ(buf.size(), 12u + 4u);  // 头 12 字节 + 1 个 FCI

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::Nack);
    EXPECT_EQ(got.nack.senderSsrc, nack.senderSsrc);
    EXPECT_EQ(got.nack.mediaSsrc, nack.mediaSsrc);
    EXPECT_EQ(got.nack.entries, nack.entries);
}

// ----------------------------------------------------------------------------
// PLI round-trip
// ----------------------------------------------------------------------------
TEST(PliPacketTest, SerializeParseRoundTrip) {
    PliPacket pli;
    pli.senderSsrc = 0x33333333;
    pli.mediaSsrc = 0x44444444;

    std::vector<uint8_t> buf;
    appendPli(buf, pli);
    EXPECT_EQ(buf.size(), 12u);  // PLI 无 FCI，固定 12 字节

    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    EXPECT_EQ(got.kind, RtcpKind::Pli);
    EXPECT_EQ(got.pli.senderSsrc, pli.senderSsrc);
    EXPECT_EQ(got.pli.mediaSsrc, pli.mediaSsrc);
}

// ----------------------------------------------------------------------------
// SDES + 复合包
// ----------------------------------------------------------------------------
TEST(CompoundTest, TwoSubPacketsIteratedInOrder) {
    SenderReport sr;
    sr.ssrc = 1;
    sr.ntpTimestamp = msToNtp(1000);
    SdesPacket sdes;
    sdes.ssrc = 1;
    sdes.cname = "crystal-test";

    std::vector<uint8_t> buf;
    appendSenderReport(buf, sr);
    appendSdes(buf, sdes);

    std::vector<RtcpKind> kinds;
    std::string cname;
    ASSERT_TRUE(parseRtcpCompound(buf.data(), buf.size(), [&](const RtcpPacket& p) {
        kinds.push_back(p.kind);
        if (p.kind == RtcpKind::Sdes) cname = p.sdes.cname;
    }));
    EXPECT_EQ(kinds, (std::vector<RtcpKind>{RtcpKind::SenderReport, RtcpKind::Sdes}));
    EXPECT_EQ(cname, "crystal-test");
}

TEST(CompoundTest, MalformedPacketRejected) {
    // length 字段声明超出实际数据 → 解析失败，不崩溃
    std::vector<uint8_t> buf = {0x80, 201, 0x00, 0x0A};  // 声明 44 字节实际只有 4
    int calls = 0;
    EXPECT_FALSE(parseRtcpCompound(buf.data(), buf.size(),
                                   [&](const RtcpPacket&) { calls++; }));
    EXPECT_EQ(calls, 0);
}
```

- [ ] **Step 5: 修改 `tests/CMakeLists.txt`**，追加：

```cmake
add_executable(crystal_rtcp_tests
    rtcp_packet_test.cpp
)
target_link_libraries(crystal_rtcp_tests PRIVATE
    crystal_media_rtcp crystal_utils GTest::gtest_main
)
add_test(NAME crystal_rtcp_tests COMMAND crystal_rtcp_tests)
```

- [ ] **Step 6: 构建并运行测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_rtcp_tests -j$(nproc) && ./crystal_rtcp_tests`
Expected: 全部 PASS

- [ ] **Step 7: 跑存量测试确认无回归**

Run: `cd /workspace/build && make crystal_rtp_tests -j$(nproc) && ./crystal_rtp_tests`
Expected: 17 用例全 PASS

- [ ] **Step 8: Commit**

```bash
git add src/media/rtcp/rtcp_packet.h src/media/rtcp/rtcp_packet.cpp \
        src/CMakeLists.txt tests/rtcp_packet_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtcp): RTCP 协议层——SR/RR/SDES/NACK/PLI 构造与解析、复合包迭代、RTP/RTCP 去复用"
```

---

### Task 2: 发送侧重传缓冲 `RetransmissionBuffer`

**Files:**
- Create: `src/media/rtcp/retransmission_buffer.h`
- Create: `src/media/rtcp/retransmission_buffer.cpp`
- Create: `tests/retransmission_buffer_test.cpp`
- Modify: `src/CMakeLists.txt`、`tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/retransmission_buffer_test.cpp`**

```cpp
// ============================================================================
// retransmission_buffer_test.cpp — 重传缓冲区单元测试
// ============================================================================
// 覆盖：命中补发字节一致、未命中计数、容量淘汰、TTL 淘汰。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/retransmission_buffer.h"

using namespace crystal;

static std::vector<uint8_t> makePacket(uint16_t seq, size_t size) {
    std::vector<uint8_t> p(size);
    p[0] = 0x80;
    p[1] = static_cast<uint8_t>(seq & 0xFF);  // 用 seq 标记内容
    return p;
}

TEST(RetransmissionBufferTest, StoreAndGetReturnsOriginalBytes) {
    RetransmissionBuffer buf(512, 3000);
    auto pkt = makePacket(100, 100);
    buf.store(100, pkt, 0);

    auto got = buf.get(100, 10);
    EXPECT_EQ(got, pkt);            // 原字节补发（同 seq 同内容）
    EXPECT_EQ(buf.retransmittedCount(), 1u);
    EXPECT_EQ(buf.missCount(), 0u);
}

TEST(RetransmissionBufferTest, MissingSeqCountsMiss) {
    RetransmissionBuffer buf(512, 3000);
    auto got = buf.get(999, 0);
    EXPECT_TRUE(got.empty());       // 未命中返回空
    EXPECT_EQ(buf.missCount(), 1u); // 计入 NACK 未命中
}

TEST(RetransmissionBufferTest, CapacityEvictsOldest) {
    RetransmissionBuffer buf(2, 3000);  // 容量 2
    auto p1 = makePacket(1, 50), p2 = makePacket(2, 50), p3 = makePacket(3, 50);
    buf.store(1, p1, 0);
    buf.store(2, p2, 0);
    buf.store(3, p3, 1);  // 触发淘汰最老的 seq=1

    EXPECT_TRUE(buf.get(1, 2).empty());  // 已被淘汰 → miss
    EXPECT_FALSE(buf.get(2, 2).empty()); // 仍在
    EXPECT_FALSE(buf.get(3, 2).empty());
    EXPECT_EQ(buf.missCount(), 1u);
}

TEST(RetransmissionBufferTest, TtlEvictsExpired) {
    RetransmissionBuffer buf(512, 300);  // TTL 300ms
    buf.store(100, makePacket(100, 50), 0);

    EXPECT_FALSE(buf.get(100, 200).empty());  // 200ms 内有效
    EXPECT_TRUE(buf.get(100, 400).empty());   // 超过 300ms → 过期淘汰
}

TEST(RetransmissionBufferTest, LateStoreOfOldSeqAllowed) {
    // 发送侧乱序（如重传与正常发送并发）也允许存储，按插入序淘汰
    RetransmissionBuffer buf(512, 3000);
    buf.store(100, makePacket(100, 50), 0);
    buf.store(99, makePacket(99, 50), 1);
    EXPECT_FALSE(buf.get(99, 2).empty());
    EXPECT_FALSE(buf.get(100, 2).empty());
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd /workspace/build && make crystal_rtcp_tests -j$(nproc) 2>&1 | head -5`
Expected: 编译失败（`retransmission_buffer.h` 不存在）

- [ ] **Step 3: 写 `src/media/rtcp/retransmission_buffer.h`**

```cpp
// ============================================================================
// retransmission_buffer.h - 发送端重传缓冲区
// ============================================================================
//
// 【职责】
// 发送端每发出一个视频 RTP 包，先存入本缓冲区。当对端 NACK 请求某个
// 序列号时，从缓冲区取出原始字节补发（同序列号、同字节，不重新打包）。
//
// 【淘汰策略——双维度】
//   1. 容量（默认 512 包）：超过容量淘汰最老包
//   2. 时间（默认 3 秒）：重传请求若晚于包的"保鲜期"（RTT + 重试窗口），
//      补发已无意义（接收端早已放弃该包），直接计 miss
//
// 【为什么不用 RTX（RFC 4588）】
// RTX 用专用 SSRC + 原始序列号重传，需要额外协商。P2P 学习项目直接
// 原样补发即可，序列号不变，接收端 JitterBuffer 无感知。
//
// 【线程模型】
// store() 在采集编码线程调用；get() 在 RTCP 接收线程调用 → 内部加锁。
// ============================================================================

#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace crystal {

class RetransmissionBuffer {
public:
    // capacity - 最大缓存包数（淘汰最老）；ttlMs - 包的保鲜期（毫秒）
    explicit RetransmissionBuffer(size_t capacity = 512, uint64_t ttlMs = 3000);

    // 存储一个已发送的 RTP 包（发出前调用）
    void store(uint16_t seq, std::vector<uint8_t> packet, uint64_t nowMs);

    // 按序列号取原始包字节用于补发
    // 返回空 vector 表示未命中（已淘汰/从未存储），计入 miss 统计
    std::vector<uint8_t> get(uint16_t seq, uint64_t nowMs);

    // 统计：成功补发次数
    uint64_t retransmittedCount() const;
    // 统计：NACK 请求但缓冲区已无此包的次数
    uint64_t missCount() const;

private:
    struct Entry {
        uint16_t seq;
        uint64_t tsMs;              // 存储时刻（用于 TTL 淘汰）
        std::vector<uint8_t> data;  // 原始 RTP 字节
    };

    // 淘汰过期/超容量条目（调用方持锁）
    void evictLocked(uint64_t nowMs);

    mutable std::mutex mutex_;
    std::deque<Entry> entries_;  // 按插入序（≈序列号递增序）
    size_t capacity_;
    uint64_t ttlMs_;
    uint64_t retransmitted_ = 0;
    uint64_t miss_ = 0;
};

} // namespace crystal
```

- [ ] **Step 4: 写 `src/media/rtcp/retransmission_buffer.cpp`**

```cpp
// ============================================================================
// retransmission_buffer.cpp - 重传缓冲区实现
// ============================================================================

#include "media/rtcp/retransmission_buffer.h"

namespace crystal {

RetransmissionBuffer::RetransmissionBuffer(size_t capacity, uint64_t ttlMs)
    : capacity_(capacity), ttlMs_(ttlMs) {}

void RetransmissionBuffer::evictLocked(uint64_t nowMs) {
    // 1. TTL 淘汰：队首最老，逐个检查
    while (!entries_.empty() && nowMs - entries_.front().tsMs > ttlMs_) {
        entries_.pop_front();
    }
    // 2. 容量淘汰：仍超容量则继续淘汰最老
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

void RetransmissionBuffer::store(uint16_t seq, std::vector<uint8_t> packet,
                                 uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    entries_.push_back({seq, nowMs, std::move(packet)});
}

std::vector<uint8_t> RetransmissionBuffer::get(uint16_t seq, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    // 512 上限内线性扫描足够（真实实现可用 hash 索引优化）
    for (const auto& e : entries_) {
        if (e.seq == seq) {
            retransmitted_++;
            return e.data;  // 拷贝原始字节
        }
    }
    miss_++;
    return {};
}

uint64_t RetransmissionBuffer::retransmittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retransmitted_;
}

uint64_t RetransmissionBuffer::missCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return miss_;
}

} // namespace crystal
```

- [ ] **Step 5: 更新 CMake**

`src/CMakeLists.txt` 中 `crystal_media_rtcp` 的源文件列表加入 `media/rtcp/retransmission_buffer.cpp`；
`tests/CMakeLists.txt` 中 `crystal_rtcp_tests` 的源文件列表加入 `retransmission_buffer_test.cpp`。

- [ ] **Step 6: 构建并运行测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_rtcp_tests -j$(nproc) && ./crystal_rtcp_tests`
Expected: Task 1 + Task 2 测试全部 PASS

- [ ] **Step 7: Commit**

```bash
git add src/media/rtcp/retransmission_buffer.h src/media/rtcp/retransmission_buffer.cpp \
        src/CMakeLists.txt tests/retransmission_buffer_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtcp): 发送侧重传缓冲区——容量+TTL 双维度淘汰，响应 NACK 原字节补发"
```

---

### Task 3: 接收端 NACK 状态机 `NackRequester`

**Files:**
- Create: `src/media/rtcp/nack_requester.h`
- Create: `src/media/rtcp/nack_requester.cpp`
- Create: `tests/nack_requester_test.cpp`
- Modify: `src/CMakeLists.txt`、`tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/nack_requester_test.cpp`**

```cpp
// ============================================================================
// nack_requester_test.cpp — NACK 请求状态机单元测试
// ============================================================================
// 覆盖：新间隙立即请求、重试节奏与上限、恢复后不再请求、放弃计数。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/nack_requester.h"

using namespace crystal;

TEST(NackRequesterTest, NewGapRequestsImmediately) {
    NackRequester nack;
    auto req = nack.onMissing({101, 102}, 0);
    EXPECT_EQ(req, (std::vector<uint16_t>{101, 102}));  // 首次请求立即返回
    EXPECT_EQ(nack.requestedCount(), 2u);
}

TEST(NackRequesterTest, RetryTooEarlyReturnsNothing) {
    NackRequester nack;
    nack.onMissing({101}, 0);
    EXPECT_TRUE(nack.tick(10).empty());   // 10ms < 33ms，未到重试时间
    EXPECT_TRUE(nack.tick(32).empty());   // 32ms 仍未到
}

TEST(NackRequesterTest, RetriesUpToLimitThenGivesUp) {
    NackRequester nack;
    nack.onMissing({101}, 0);                                 // 第 1 次请求
    EXPECT_EQ(nack.tick(33), (std::vector<uint16_t>{101}));   // 第 2 次（重试 1）
    EXPECT_EQ(nack.tick(66), (std::vector<uint16_t>{101}));   // 第 3 次（重试 2）
    EXPECT_TRUE(nack.tick(99).empty());       // 已达 3 次上限 → 放弃
    EXPECT_EQ(nack.givenUpCount(), 1u);
    EXPECT_EQ(nack.requestedCount(), 3u);     // 总请求次数 = 3
    EXPECT_TRUE(nack.tick(500).empty());      // 放弃后不再出现
}

TEST(NackRequesterTest, RecoveredPacketNotRequestedAgain) {
    NackRequester nack;
    nack.onMissing({101, 102}, 0);
    nack.onReceived(101);                     // 101 经重传/乱序到达
    auto retry = nack.tick(33);
    EXPECT_EQ(retry, (std::vector<uint16_t>{102}));  // 只重试 102
    EXPECT_EQ(nack.givenUpCount(), 0u);
}

TEST(NackRequesterTest, DuplicateGapNotReRegistered) {
    NackRequester nack;
    nack.onMissing({101}, 0);
    auto again = nack.onMissing({101}, 5);    // 同一 seq 重复上报
    EXPECT_TRUE(again.empty());               // 已在表中，不重复立即请求
    EXPECT_EQ(nack.requestedCount(), 1u);
}

TEST(NackRequesterTest, MixedTimelineWorks) {
    NackRequester nack;
    nack.onMissing({200, 201, 202}, 0);
    nack.onReceived(201);
    auto r1 = nack.tick(34);
    EXPECT_EQ(r1, (std::vector<uint16_t>{200, 202}));
    nack.onReceived(200);
    auto r2 = nack.tick(68);
    EXPECT_EQ(r2, (std::vector<uint16_t>{202}));
    nack.onReceived(202);                     // 全部恢复，无放弃
    EXPECT_TRUE(nack.tick(102).empty());
    EXPECT_EQ(nack.givenUpCount(), 0u);
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd /workspace/build && make crystal_rtcp_tests -j$(nproc) 2>&1 | head -5`
Expected: 编译失败（`nack_requester.h` 不存在）

- [ ] **Step 3: 写 `src/media/rtcp/nack_requester.h`**

```cpp
// ============================================================================
// nack_requester.h - 接收端 NACK 请求状态机
// ============================================================================
//
// 【职责】
// 接收 JitterBuffer 检测到的丢失序列号，管理"请求 → 重试 → 放弃"状态：
//   新间隙   → 立即请求（onMissing 返回值即本次要发的 seq）
//   未恢复   → 每 33ms（约一个视频帧周期）重试，上限 3 次
//   超上限   → 放弃（大概率真丢包，画面恢复交给 PLI 兜底）
//   包到达   → 移除待请求条目（重传成功或原本乱序）
//
// 【参数权衡（面试考点）】
//   重试上限 3：NACK 往返一次 ≈ RTT；30fps 下 33ms 间隔 × 3 次 ≈ 100ms，
//   超过此窗口接收端缓冲早已输出该时间片，继续重传无意义。
//
// 【驱动方式】
//   onMissing/onReceived 由接收路径（onTrack 回调）调用；
//   tick 由上层周期调用（main_client 在每个 RTP 包到达时调用，
//   有包到达才可能需要重试）。
// ============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace crystal {

class NackRequester {
public:
    static constexpr int kMaxRetries = 3;          // 含首次请求的总次数
    static constexpr uint64_t kRetryIntervalMs = 33;

    // 登记新检测到的丢失 seq；返回需要"立即"请求的 seq（首次请求）
    std::vector<uint16_t> onMissing(const std::vector<uint16_t>& gaps,
                                    uint64_t nowMs);
    // 周期驱动；返回到期需要重试的 seq
    std::vector<uint16_t> tick(uint64_t nowMs);
    // 任意 RTP 包到达（含重传/乱序）时调用，解除待请求
    void onReceived(uint16_t seq);

    // 统计：累计发出的请求次数（含重试）
    uint64_t requestedCount() const;
    // 统计：重试耗尽仍未恢复、最终放弃的 seq 数
    uint64_t givenUpCount() const;

private:
    struct Item {
        uint8_t tries = 1;        // 已请求次数（登记即第 1 次）
        uint64_t lastReqMs = 0;   // 上次请求时刻
    };

    mutable std::mutex mutex_;
    std::map<uint16_t, Item> pending_;  // key = 丢失序列号
    uint64_t requested_ = 0;
    uint64_t givenUp_ = 0;
};

} // namespace crystal
```

- [ ] **Step 4: 写 `src/media/rtcp/nack_requester.cpp`**

```cpp
// ============================================================================
// nack_requester.cpp - NACK 请求状态机实现
// ============================================================================

#include "media/rtcp/nack_requester.h"

namespace crystal {

std::vector<uint16_t> NackRequester::onMissing(const std::vector<uint16_t>& gaps,
                                               uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (uint16_t seq : gaps) {
        auto it = pending_.find(seq);
        if (it != pending_.end()) continue;  // 已在表中，交给 tick 重试
        pending_[seq] = Item{1, nowMs};      // 登记即第 1 次请求
        requested_++;
        toRequest.push_back(seq);
    }
    return toRequest;
}

std::vector<uint16_t> NackRequester::tick(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowMs - it->second.lastReqMs < kRetryIntervalMs) {
            ++it;  // 未到重试时间
            continue;
        }
        if (it->second.tries >= kMaxRetries) {
            // 重试耗尽仍未恢复 → 放弃（PLI 兜底恢复画面）
            givenUp_++;
            it = pending_.erase(it);
            continue;
        }
        it->second.tries++;
        it->second.lastReqMs = nowMs;
        requested_++;
        toRequest.push_back(it->first);
        ++it;
    }
    return toRequest;
}

void NackRequester::onReceived(uint16_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(seq);
}

uint64_t NackRequester::requestedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_;
}

uint64_t NackRequester::givenUpCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return givenUp_;
}

} // namespace crystal
```

- [ ] **Step 5: 更新 CMake**

`src/CMakeLists.txt` 的 `crystal_media_rtcp` 源文件加入 `media/rtcp/nack_requester.cpp`；
`tests/CMakeLists.txt` 的 `crystal_rtcp_tests` 源文件加入 `nack_requester_test.cpp`。

- [ ] **Step 6: 构建并运行测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_rtcp_tests -j$(nproc) && ./crystal_rtcp_tests`
Expected: 全部 PASS

- [ ] **Step 7: Commit**

```bash
git add src/media/rtcp/nack_requester.h src/media/rtcp/nack_requester.cpp \
        src/CMakeLists.txt tests/nack_requester_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtcp): 接收端 NACK 状态机——立即请求+33ms 重试+上限放弃"
```

---

### Task 4: 统计 `RtcpReporter`（SendSide + RecvSide）

**Files:**
- Create: `src/media/rtcp/rtcp_reporter.h`
- Create: `src/media/rtcp/rtcp_reporter.cpp`
- Create: `tests/rtcp_reporter_test.cpp`
- Modify: `src/CMakeLists.txt`、`tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/rtcp_reporter_test.cpp`**

```cpp
// ============================================================================
// rtcp_reporter_test.cpp — SR/RR 统计器单元测试
// ============================================================================
// 覆盖：接收侧丢包/回绕/抖动（RFC 3550 A.1/A.8）、发送侧计数、RTT 计算
// （LSR/DLSR 法，注入时钟避免真实时间）。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/rtcp_reporter.h"

using namespace crystal;

// 毫秒 → NTP（测试辅助）
static uint64_t msToNtp(uint64_t ms) {
    return (static_cast<uint64_t>(ms) << 32) / 1000;
}

// ----------------------------------------------------------------------------
// RecvSideReporter：丢包统计（RFC 3550 A.1）
// ----------------------------------------------------------------------------
TEST(RecvSideTest, LossAndFractionLost) {
    RecvSideReporter recv(90000);  // 视频时钟
    // 收到 seq 1,2,3,4,6,7,8,9,10（5 丢失），乱序喂入
    uint16_t seqs[] = {2, 1, 4, 3, 6, 8, 7, 10, 9};
    uint64_t t = 0;
    for (uint16_t s : seqs) {
        recv.onPacketReceived(0xAA000001, s, s * 3000, msToNtp(t));
        t += 33;
    }

    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(t), blk));
    // expected = 10-1+1 = 10, received = 9 → 累计丢 1
    EXPECT_EQ(blk.cumulativeLost, 1u);
    // 间隔统计：期望增量 10，接收增量 9，丢 1 → fraction = 1*256/10 = 25
    EXPECT_EQ(blk.fractionLost, 25u);
    EXPECT_DOUBLE_EQ(recv.lossRate(), 0.1);  // 1/10
}

TEST(RecvSideTest, SequenceWraparound) {
    RecvSideReporter recv(90000);
    recv.onPacketReceived(1, 65534, 0, msToNtp(0));
    recv.onPacketReceived(1, 65535, 3000, msToNtp(33));
    recv.onPacketReceived(1, 0, 6000, msToNtp(66));     // 回绕
    recv.onPacketReceived(1, 1, 9000, msToNtp(99));

    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(132), blk));
    // 扩展最高序列号 = 65536+1 = 65537；期望 4 收 4 → 无丢失
    EXPECT_EQ(blk.extHighestSeq, 65537u);
    EXPECT_EQ(blk.cumulativeLost, 0u);
}

TEST(RecvSideTest, JitterFollowsTransitVariation) {
    RecvSideReporter recv(1000);  // 时钟 1000Hz，1 tick = 1ms
    // 包间隔 20ms、RTP ts 增量 20 → transit 恒定，抖动为 0
    recv.onPacketReceived(1, 1, 0,  msToNtp(0));
    recv.onPacketReceived(1, 2, 20, msToNtp(20));
    EXPECT_DOUBLE_EQ(recv.jitterMs(), 0.0);
    // 第 3 包晚到 10ms → D=10 → jitter = (10-0)/16 = 0.625ms
    recv.onPacketReceived(1, 3, 40, msToNtp(70));
    EXPECT_NEAR(recv.jitterMs(), 0.625, 0.01);
}

TEST(RecvSideTest, LsrDlsrFromSenderReport) {
    RecvSideReporter recv(90000);
    recv.onPacketReceived(0xAA000001, 1, 3000, msToNtp(0));  // 激活该流
    recv.onSenderReport(0xAA000001, msToNtp(1000));  // 记住 SR 到达时刻
    ReportBlock blk;
    ASSERT_TRUE(recv.buildBlock(msToNtp(2500), blk));
    EXPECT_EQ(blk.lsr, ntpMiddle32(msToNtp(1000)));
    // 1500ms = 1.5s → DLSR = 1.5 * 65536 = 98304
    EXPECT_EQ(blk.dlsr, 98304u);
}

// ----------------------------------------------------------------------------
// SendSideReporter：计数与 RTT
// ----------------------------------------------------------------------------
TEST(SendSideTest, CountsAndSenderReportRoundTrip) {
    SendSideReporter send(0x12345678, "crystal-test");
    send.onPacketSent(100, 1500, 3000);
    send.onPacketSent(101, 1200, 6000);

    auto report = send.buildReport(msToNtp(5000), {});  // 无接收报告块
    RtcpPacket got;
    ASSERT_TRUE(parseRtcpCompound(report.data(), report.size(),
                                  [&](const RtcpPacket& p) { got = p; }));
    ASSERT_EQ(got.kind, RtcpKind::SenderReport);
    EXPECT_EQ(got.sr.ssrc, 0x12345678u);
    EXPECT_EQ(got.sr.packetCount, 2u);
    EXPECT_EQ(got.sr.octetCount, 2700u);
    EXPECT_EQ(got.sr.rtpTimestamp, 6000u);  // 最近一个包的 ts
}

TEST(SendSideTest, RttFromReceiverReport) {
    SendSideReporter send(0x12345678, "c");
    uint64_t srNtp = msToNtp(10000);        // 我方 SR 发出（对端以此填 LSR）
    ReceiverReport rr;
    rr.ssrc = 0x87654321;
    ReportBlock blk;
    blk.ssrc = 0x12345678;                  // 针对我方视频流
    blk.lsr = ntpMiddle32(srNtp);           // 对端收到我方 SR 的时刻
    blk.dlsr = 65536;                       // 对端延迟 1.0s 后发出 RR
    rr.blocks.push_back(blk);

    // 我方在 SR 发出 2.0s 后收到 RR → RTT = 2.0 - 1.0 = 1.0s
    send.onReceiverReport(rr, srNtp + (2ULL << 32));
    EXPECT_TRUE(send.hasRtt());
    EXPECT_NEAR(send.rttMs(), 1000.0, 1.0);
}

TEST(SendSideTest, RttSkippedWhenLsrZero) {
    SendSideReporter send(0x12345678, "c");
    ReceiverReport rr;
    rr.ssrc = 0x87654321;
    ReportBlock blk;
    blk.ssrc = 0x12345678;
    blk.lsr = 0;  // 对端从未收到我方 SR
    rr.blocks.push_back(blk);

    send.onReceiverReport(rr, msToNtp(1000));
    EXPECT_FALSE(send.hasRtt());
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd /workspace/build && make crystal_rtcp_tests -j$(nproc) 2>&1 | head -5`
Expected: 编译失败（`rtcp_reporter.h` 不存在）

- [ ] **Step 3: 写 `src/media/rtcp/rtcp_reporter.h`**

```cpp
// ============================================================================
// rtcp_reporter.h - SR/RR 统计器（发送侧 + 接收侧）
// ============================================================================
//
// 【SendSideReporter —— 发送侧，每条发送流一个实例】
//   记录发包计数；周期构造 SR 复合包（SR + SDES）；
//   解析对端 RR 的 LSR/DLSR 计算 RTT：
//     RTT = 收到 RR 的 NTP 时刻 - LSR - DLSR   （RFC 3550 A.6）
//   【面试考点】RTT 只能在发送侧算：LSR 是"对端收到我方 SR"的时刻，
//   DLSR 是"对端从收 SR 到发 RR"的间隔，两者之差被 NTP 时钟对齐后
//   才能减出网络往返时间。
//
// 【RecvSideReporter —— 接收侧，每条接收流一个实例】
//   每个 RTP 包到达时更新丢包（RFC 3550 A.1）与抖动（A.8）统计；
//   周期产出 ReportBlock（可同时嵌入 RR 与我方 SR）。
//
// 【抖动算法（RFC 3550 A.8）】
//   transit = 到达时刻(RTP 单位) - RTP 时间戳
//   D = |transit_i - transit_{i-1}|
//   jitter += (D - jitter) / 16
//   除以 16 相当于指数平滑（增益 1/16），既跟得上波动又抗毛刺。
// ============================================================================

#pragma once

#include "media/rtcp/rtcp_packet.h"
#include <mutex>
#include <string>
#include <vector>

namespace crystal {

// ============================================================================
// SendSideReporter
// ============================================================================
class SendSideReporter {
public:
    SendSideReporter(uint32_t ssrc, std::string cname);

    // 每发出一个 RTP 包调用（payloadBytes = 负载字节数，不含 RTP 头）
    void onPacketSent(uint16_t seq, size_t payloadBytes, uint32_t rtpTs);

    // 构造 SR 复合包（SR + SDES），blocks 为我方对对端流的接收统计
    std::vector<uint8_t> buildReport(uint64_t nowNtp,
                                     const std::vector<ReportBlock>& blocks);

    // 收到对端 RR 时调用（自动匹配 blocks 中 ssrc == 本流的报告块）
    void onReceiverReport(const ReceiverReport& rr, uint64_t nowNtp);

    uint32_t ssrc() const;
    bool hasRtt() const;       // 是否已算出 RTT
    double rttMs() const;      // RTT（毫秒）
    uint8_t remoteFractionLost() const;  // 对端观测的我方丢包率（256=100%）

private:
    uint32_t ssrc_;
    std::string cname_;
    mutable std::mutex mutex_;
    uint32_t packetCount_ = 0;
    uint32_t octetCount_ = 0;
    uint32_t lastRtpTs_ = 0;
    double rttMs_ = -1;
    uint8_t remoteFractionLost_ = 0;
};

// ============================================================================
// RecvSideReporter
// ============================================================================
class RecvSideReporter {
public:
    // clockRate - 该流的 RTP 时钟（视频 90000 / 音频 48000），抖动换算用
    explicit RecvSideReporter(uint32_t clockRate);

    // 每个 RTP 包到达时调用；首个包自动采纳该流的 SSRC
    void onPacketReceived(uint32_t ssrc, uint16_t seq, uint32_t rtpTs,
                          uint64_t arrivalNtp);

    // 收到对端 SR 时调用（记录 LSR 基准时刻）
    void onSenderReport(uint32_t ssrc, uint64_t arrivalNtp);

    // 产出报告块（嵌入 RR 或 SR）；流未激活时返回 false
    bool buildBlock(uint64_t nowNtp, ReportBlock& out);

    bool active() const;           // 是否已收到过包
    uint32_t remoteSsrc() const;   // 对端流 SSRC
    double lossRate() const;       // 累计丢包率 [0,1]
    double jitterMs() const;       // 平滑抖动（毫秒）

private:
    uint32_t clockRate_;
    mutable std::mutex mutex_;
    uint32_t ssrc_ = 0;
    bool active_ = false;
    uint16_t baseSeq_ = 0;       // 首包序列号（期望包数基准）
    uint16_t maxSeq_ = 0;        // 最高序列号
    uint32_t cycles_ = 0;        // 回绕计数（<<16 进扩展序列号）
    uint64_t received_ = 0;
    uint64_t expectedPrior_ = 0; // 上份报告的期望包数（fraction 增量用）
    uint64_t receivedPrior_ = 0; // 上份报告的接收包数
    double jitter_ = 0;          // 平滑抖动（RTP 时间单位）
    int64_t lastTransit_ = 0;
    uint64_t lastSrNtp_ = 0;     // 最近收到对端 SR 的 NTP 时刻
};

} // namespace crystal
```

- [ ] **Step 4: 写 `src/media/rtcp/rtcp_reporter.cpp`**

```cpp
// ============================================================================
// rtcp_reporter.cpp - SR/RR 统计器实现
// ============================================================================

#include "media/rtcp/rtcp_reporter.h"
#include <algorithm>

namespace crystal {

// ============================================================================
// SendSideReporter
// ============================================================================
SendSideReporter::SendSideReporter(uint32_t ssrc, std::string cname)
    : ssrc_(ssrc), cname_(std::move(cname)) {}

void SendSideReporter::onPacketSent(uint16_t /*seq*/, size_t payloadBytes,
                                    uint32_t rtpTs) {
    std::lock_guard<std::mutex> lock(mutex_);
    packetCount_++;
    octetCount_ += static_cast<uint32_t>(payloadBytes);
    lastRtpTs_ = rtpTs;
}

std::vector<uint8_t> SendSideReporter::buildReport(
    uint64_t nowNtp, const std::vector<ReportBlock>& blocks) {
    std::lock_guard<std::mutex> lock(mutex_);
    SenderReport sr;
    sr.ssrc = ssrc_;
    sr.ntpTimestamp = nowNtp;
    sr.rtpTimestamp = lastRtpTs_;
    sr.packetCount = packetCount_;
    sr.octetCount = octetCount_;
    sr.blocks = blocks;

    // RFC 3550 6.4.1：SR 之后必须跟 SDES（CNAME）
    std::vector<uint8_t> out;
    appendSenderReport(out, sr);
    SdesPacket sdes;
    sdes.ssrc = ssrc_;
    sdes.cname = cname_;
    appendSdes(out, sdes);
    return out;
}

void SendSideReporter::onReceiverReport(const ReceiverReport& rr,
                                        uint64_t nowNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& blk : rr.blocks) {
        if (blk.ssrc != ssrc_) continue;  // 只认针对本流的报告块
        remoteFractionLost_ = blk.fractionLost;
        if (blk.lsr != 0) {
            // RTT = A - LSR - DLSR（A = 本机当前 NTP，全部为中间 32 位格式，
            // 32 位无符号回绕减法天然正确）
            uint64_t nowMid = (nowNtp >> 16) & 0xFFFFFFFF;
            uint32_t rttMid = static_cast<uint32_t>(nowMid - blk.lsr - blk.dlsr);
            rttMs_ = rttMid / 65536.0 * 1000.0;
        }
    }
}

uint32_t SendSideReporter::ssrc() const { return ssrc_; }

bool SendSideReporter::hasRtt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rttMs_ >= 0;
}

double SendSideReporter::rttMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rttMs_;
}

uint8_t SendSideReporter::remoteFractionLost() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return remoteFractionLost_;
}

// ============================================================================
// RecvSideReporter
// ============================================================================
RecvSideReporter::RecvSideReporter(uint32_t clockRate) : clockRate_(clockRate) {}

void RecvSideReporter::onPacketReceived(uint32_t ssrc, uint16_t seq,
                                        uint32_t rtpTs, uint64_t arrivalNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) {
        // 首包：采纳 SSRC，初始化序列号基准（RFC 3550 A.1）
        ssrc_ = ssrc;
        baseSeq_ = seq;
        maxSeq_ = seq;
        cycles_ = 0;
        active_ = true;
    } else if (ssrc != ssrc_) {
        return;  // 非本流包（每实例只跟踪一条流）
    }

    // 更新最高序列号与回绕计数
    // int16_t 差值正确处理回绕（与 JitterBuffer 同技巧）
    int16_t delta = static_cast<int16_t>(seq - maxSeq_);
    if (delta > 0) {
        if (seq < maxSeq_) cycles_ += 65536;  // 正向跳变且值变小 → 回绕
        maxSeq_ = seq;
    }
    received_++;

    // 抖动更新（RFC 3550 A.8）：到达时刻从 NTP 换算到 RTP 单位
    uint64_t arrivalRtp =
        ((arrivalNtp >> 16) * static_cast<uint64_t>(clockRate_)) / 65536;
    int64_t transit = static_cast<int64_t>(arrivalRtp) - static_cast<int64_t>(rtpTs);
    int64_t d = transit - lastTransit_;
    if (d < 0) d = -d;
    jitter_ += (d - jitter_) / 16.0;
    lastTransit_ = transit;
}

void RecvSideReporter::onSenderReport(uint32_t ssrc, uint64_t arrivalNtp) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 只记录本流的 SR：未激活（尚不知道本流 SSRC）时不能记录，
    // 否则会把其他流的 SR 错误归到本流，导致 LSR/DLSR 计算错误
    if (active_ && ssrc == ssrc_) {
        lastSrNtp_ = arrivalNtp;  // 记录 LSR 基准（供报告块 DLSR 用）
    }
}

bool RecvSideReporter::buildBlock(uint64_t nowNtp, ReportBlock& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return false;

    // RFC 3550 A.1：期望包数与丢包
    uint32_t extMax = cycles_ + maxSeq_;
    uint64_t expected = extMax - baseSeq_ + 1;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    if (lost < 0) lost = 0;

    // 间隔丢包率（fraction lost，自上份报告以来的增量）
    int64_t expectedInterval = static_cast<int64_t>(expected - expectedPrior_);
    int64_t receivedInterval = static_cast<int64_t>(received_ - receivedPrior_);
    int64_t lostInterval = expectedInterval - receivedInterval;
    uint8_t fraction = 0;
    if (expectedInterval > 0 && lostInterval > 0) {
        fraction = static_cast<uint8_t>(
            std::min(255.0, lostInterval * 256.0 / expectedInterval));
    }
    expectedPrior_ = expected;
    receivedPrior_ = received_;

    out = ReportBlock{};
    out.ssrc = ssrc_;
    out.fractionLost = fraction;
    out.cumulativeLost = static_cast<uint32_t>(
        std::min<uint64_t>(0xFFFFFF, static_cast<uint64_t>(lost)));
    out.extHighestSeq = extMax;
    out.jitter = static_cast<uint32_t>(jitter_);
    // LSR/DLSR：对端最近一次 SR 的到达时刻
    if (lastSrNtp_ != 0) {
        out.lsr = ntpMiddle32(lastSrNtp_);
        uint64_t delayNtp = nowNtp - lastSrNtp_;
        out.dlsr = static_cast<uint32_t>((delayNtp >> 16) & 0xFFFFFFFF);
    }
    return true;
}

bool RecvSideReporter::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

uint32_t RecvSideReporter::remoteSsrc() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssrc_;
}

double RecvSideReporter::lossRate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return 0.0;
    uint32_t extMax = cycles_ + maxSeq_;
    uint64_t expected = extMax - baseSeq_ + 1;
    if (expected == 0) return 0.0;
    int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
    if (lost <= 0) return 0.0;
    return static_cast<double>(lost) / static_cast<double>(expected);
}

double RecvSideReporter::jitterMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jitter_ / static_cast<double>(clockRate_) * 1000.0;
}

} // namespace crystal
```

- [ ] **Step 5: 更新 CMake**

`src/CMakeLists.txt` 的 `crystal_media_rtcp` 源文件加入 `media/rtcp/rtcp_reporter.cpp`；
`tests/CMakeLists.txt` 的 `crystal_rtcp_tests` 源文件加入 `rtcp_reporter_test.cpp`。

- [ ] **Step 6: 构建并运行测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_rtcp_tests -j$(nproc) && ./crystal_rtcp_tests`
Expected: 全部 PASS。特别注意 `RecvSideTest.LossAndFractionLost` 的 fraction=25、`SendSideTest.RttFromReceiverReport` 的 RTT≈1000ms。

- [ ] **Step 7: Commit**

```bash
git add src/media/rtcp/rtcp_reporter.h src/media/rtcp/rtcp_reporter.cpp \
        src/CMakeLists.txt tests/rtcp_reporter_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtcp): SR/RR 统计器——RFC 3550 丢包/抖动/回绕 + LSR/DLSR 法 RTT"
```

---

### Task 5: `JitterBuffer::insert()` 返回丢失序列号

**Files:**
- Modify: `src/media/rtp/jitter_buffer.h`
- Modify: `src/media/rtp/jitter_buffer.cpp`
- Create: `tests/jitter_buffer_gap_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/jitter_buffer_gap_test.cpp`**

```cpp
// ============================================================================
// jitter_buffer_gap_test.cpp — insert() 丢失序列号返回值测试
// ============================================================================
// 覆盖：间隙返回缺失 seq、大间隙不上报（防 NACK 风暴）、迟到包不触发。
// 注：RtpPacket 的 setter（setPayloadType 等）已存在，见 rtp_packet.h。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtp/jitter_buffer.h"

using namespace crystal;

static RtpPacket makePkt(uint16_t seq, uint8_t pt = 96) {
    RtpPacket p;
    p.setPayloadType(pt);
    p.setSequenceNumber(seq);
    p.setTimestamp(seq * 3000);
    p.setSsrc(0x12345678);
    std::vector<uint8_t> payload(10, 0xAB);
    p.setPayload(payload);
    return p;
}

TEST(JitterBufferGapTest, ReturnsMissingSeqsOnGap) {
    JitterBuffer jb;
    EXPECT_TRUE(jb.insert(makePkt(100)).empty());        // 首包无间隙
    EXPECT_EQ(jb.insert(makePkt(103)),                   // 缺 101,102
              (std::vector<uint16_t>{101, 102}));
    EXPECT_EQ(jb.insert(makePkt(105)),                   // 缺 104
              (std::vector<uint16_t>{104}));
}

TEST(JitterBufferGapTest, LatePacketReportsNothing) {
    JitterBuffer jb;
    jb.insert(makePkt(100));
    jb.insert(makePkt(103));  // 101,102 报丢失
    EXPECT_TRUE(jb.insert(makePkt(101)).empty());  // 迟到包只是恢复
}

TEST(JitterBufferGapTest, HugeGapNotReported) {
    JitterBuffer jb;
    jb.insert(makePkt(100));
    // diff=99 > kMaxNackGap(64)：疑似码流重启/暂停，不做 NACK
    auto missing = jb.insert(makePkt(200));
    EXPECT_TRUE(missing.empty());
    // 但丢包统计仍然计数
    EXPECT_EQ(jb.lostPacketCount(), 99u);
}

TEST(JitterBufferGapTest, InOrderNoGap) {
    JitterBuffer jb;
    jb.insert(makePkt(1000));
    jb.insert(makePkt(1001));
    jb.insert(makePkt(1002));
    EXPECT_TRUE(jb.insert(makePkt(1003)).empty());
}
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd /workspace/build && make crystal_rtcp_tests -j$(nproc) 2>&1 | head -5`
Expected: 编译失败（`insert` 返回 `void`，不能用于 `EXPECT_TRUE`）

- [ ] **Step 3: 修改 `src/media/rtp/jitter_buffer.h`**

3a. 类内 `insert` 声明替换为（含注释更新）：

```cpp
    // 向缓冲区插入一个 RTP 包
    // 参数:
    //   pkt - 接收到的 RTP 包
    // 返回值:
    //   本次新检测到的丢失序列号列表（供 NACK 请求使用）
    //   - 间隙 ≤ kMaxNackGap：返回全部缺失 seq（重传有意义）
    //   - 间隙 > kMaxNackGap：返回空（疑似码流暂停/重连，请求重传只会
    //     造成 NACK 风暴，画面恢复交给 PLI）
    // 核心逻辑:
    //   1. 第一个包：初始化 expectedSeq_，直接插入
    //   2. diff == 0: 收到期望的包，正常插入
    //   3. diff > 0:  收到未来的包，检测间隙（丢包），插入并报告缺失 seq
    //   4. diff < 0:  收到迟到/重复的包，仅插入（不重复计数）
    std::vector<uint16_t> insert(const RtpPacket& pkt);
```

3b. private 成员区顶部（`targetDelayMs_` 之前）加常量：

```cpp
    // 间隙上报上限：超过此值的序列号跳变不生成 NACK 请求
    // （正常网络抖动不会连续丢几十个包，大间隙意味着码流中断）
    static constexpr int kMaxNackGap = 64;
```

- [ ] **Step 4: 修改 `src/media/rtp/jitter_buffer.cpp` 的 `insert()`**

函数签名改为 `std::vector<uint16_t> JitterBuffer::insert(const RtpPacket& pkt)`，函数体开头加 `std::vector<uint16_t> missing;`，首包分支的 `return;` 改为 `return missing;`；`diff > 0` 分支替换为：

```cpp
    } else if (diff > 0) {
        // ====================================================================
        // 收到未来的包（seq > expectedSeq_）
        // 缺失的包数 = diff（即 seq - expectedSeq_）
        // 例如: expectedSeq_=101, seq=103 → 缺失 101,102
        // ====================================================================
        lostCount_ += static_cast<uint64_t>(diff);
        if (diff <= kMaxNackGap) {
            // 小间隙：把缺失的 seq 报告给上层做 NACK 重传请求
            for (int i = 0; i < diff; i++) {
                missing.push_back(static_cast<uint16_t>(expectedSeq_ + i));
            }
        } else {
            Logger::warn("JitterBuffer: huge gap ({}), skip NACK report", diff);
        }
        Logger::debug("JitterBuffer: gap detected, expected {} got {}, {} lost",
                      expectedSeq_, seq, diff);
        buffer_[seq] = pkt;
        expectedSeq_ = seq + 1;
    } else {
```

其余分支（`diff == 0`、`diff < 0`）末尾统一 `return missing;`（函数收尾处一个 return 即可）。

- [ ] **Step 5: 更新 `tests/CMakeLists.txt`**

`crystal_rtcp_tests` 源文件加入 `jitter_buffer_gap_test.cpp`，链接库改为 `crystal_media_rtcp crystal_media_rtp crystal_utils GTest::gtest_main`（该测试用到 JitterBuffer）。

- [ ] **Step 6: 构建并跑全部测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_rtcp_tests crystal_rtp_tests -j$(nproc) && ./crystal_rtcp_tests && ./crystal_rtp_tests`
Expected: 两者全部 PASS（存量 jitter_buffer_test.cpp 调用 `insert()` 忽略返回值，不受签名变化影响）

- [ ] **Step 7: Commit**

```bash
git add src/media/rtp/jitter_buffer.h src/media/rtp/jitter_buffer.cpp \
        tests/jitter_buffer_gap_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtp): JitterBuffer::insert 返回新检测丢失 seq，大间隙防 NACK 风暴"
```

### Task 6: 编码器 `forceKeyframe()` 与 解码器 `onError`（PLI 响应链路）

**Files:**
- Modify: `src/media/video/h264_encoder.h`
- Modify: `src/media/video/h264_encoder.cpp`
- Modify: `src/media/video/h264_decoder.h`
- Modify: `src/media/video/h264_decoder.cpp`

说明：本任务无独立单元测试（编码全流程需 YUV 输入，属集成验证范畴，由 Task 8 的 `crystal_client` 构建与 Task 9 的 `tc netem` 手测覆盖）。验证标准 = 编译通过 + 存量测试保持全绿。

- [ ] **Step 1: `h264_encoder.h` 新增 `forceKeyframe()`**

public 区（`onEncoded` 之后）加：

```cpp
    // 请求下一帧强制编码为 IDR 关键帧（响应 RTCP PLI）
    // 场景：对端解码失败/重传放弃后调用，关键帧可独立解码，使画面立即恢复。
    // 幂等：重复调用在下一帧只产生一个 IDR
    void forceKeyframe();
```

private 成员区（`encodedCb_` 之后）加：

```cpp
    bool forceKeyframe_ = false;    // 关键帧请求标志（encode 时消费并复位）
```

- [ ] **Step 2: `h264_encoder.cpp` 实现**

`onEncoded()` 实现之后加：

```cpp
// ============================================================================
// H264Encoder::forceKeyframe() - 请求下一帧强制 IDR
// ============================================================================
// 仅置标志，真正的动作在 encode() 中执行（编码由采集帧驱动，调用方无法
// 直接触发一次编码）。这是"命令-检查"模式的典型用法，跨线程也安全：
// 本端编码线程是唯一写者，标志本身是原子语义的 bool。
void H264Encoder::forceKeyframe() {
    forceKeyframe_ = true;
}
```

`encode()` 中 `frame_->pts = pts_++;` 之后插入：

```cpp
    // 关键帧强制（PLI 响应）：pict_type 显式指定 IDR 后由 libx264 执行
    // 注意：frame_ 是复用对象，必须每帧重置，否则标志消费一次后会
    // 永久保持 IDR（每帧都是 I 帧，码率暴涨）
    if (forceKeyframe_) {
        frame_->pict_type = AV_PICTURE_TYPE_IDR;
        forceKeyframe_ = false;
        Logger::info("Encoder: forcing keyframe (PLI response)");
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;  // 交回编码器按 GOP 决定
    }
```

- [ ] **Step 3: `h264_decoder.h` 新增错误回调**

public 区（`onDecoded` 之后）加：

```cpp
    // 解码错误回调类型（无参数：调用方只需知道"该请求关键帧了"）
    using ErrorCallback = std::function<void()>;

    // 注册解码错误回调
    // 触发时机：
    //   1. avcodec_send_packet 返回错误（码流严重损坏无法送入）
    //   2. 解码输出的帧带 decode_error_flags（FFmpeg 用错误隐藏技术
    //      "猜"出来的帧，视觉上通常表现为花屏/绿屏）
    // 上层典型处理：节流后发 RTCP PLI 请求对端编出关键帧
    void onError(ErrorCallback cb);
```

private 成员区（`decodedCb_` 之后）加：

```cpp
    ErrorCallback errorCb_;             // 解码错误回调函数
```

- [ ] **Step 4: `h264_decoder.cpp` 实现两处触发**

4a. `decode()` 中 `avcodec_send_packet` 失败分支：

```cpp
    int ret = avcodec_send_packet(codecCtx_, pkt_);
    if (ret < 0) {
        Logger::debug("Failed to send packet to decoder: {}", ret);
        if (errorCb_) errorCb_();  // 码流损坏 → 通知上层请求关键帧
        return;
    }
```

4b. `while (avcodec_receive_frame(...))` 循环体内，`decodedCb_` 调用之后加：

```cpp
        // 错误隐藏检测：FFmpeg 对损坏的 P 帧会输出"修补"帧而非报错，
        // decode_error_flags 非零表示该帧存在错误隐藏（花屏风险）
        if (frame_->decode_error_flags != 0 && errorCb_) {
            errorCb_();
        }
```

文件末尾 `onDecoded()` 之后加：

```cpp
// ============================================================================
// H264Decoder::onError() - 注册解码错误回调
// ============================================================================
void H264Decoder::onError(ErrorCallback cb) {
    errorCb_ = std::move(cb);
}
```

- [ ] **Step 5: 构建验证（全部目标）**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_media_video crystal_client -j$(nproc) 2>&1 | tail -3 && ./crystal_rtp_tests`
Expected: 编译成功；存量 17 用例仍全绿

- [ ] **Step 6: Commit**

```bash
git add src/media/video/h264_encoder.h src/media/video/h264_encoder.cpp \
        src/media/video/h264_decoder.h src/media/video/h264_decoder.cpp
git commit -m "feat(video): 编码器 forceKeyframe 与解码器 onError，打通 PLI 响应链路"
```

---

### Task 7: `PeerConnection` RTP/RTCP 去复用与 `sendRtcp`

**Files:**
- Modify: `src/transport/transport_manager.h`
- Modify: `src/transport/transport_manager.cpp`
- Modify: `src/CMakeLists.txt`（crystal_transport 链接 crystal_media_rtcp）

说明：去复用判定 `isRtcpPacket()` 已由 Task 1 实现并测试。本任务无独立单元测试（需要 libdatachannel 网络栈），验证 = 编译 + Task 8 集成。

- [ ] **Step 1: `transport_manager.h` 新增 RTCP 接口**

public 区 `TrackCallback` 别名之后加：

```cpp
    // RTCP 数据接收回调函数类型
    // PeerConnection 内部已按 RFC 5761 完成 RTP/RTCP 去复用，
    // 本回调只收到 RTCP 报文（可能是含多个子包的复合包）
    using RtcpCallback = std::function<void(const std::vector<uint8_t>& data)>;
```

public 区 `sendMedia(vector 版本)` 之后加：

```cpp
    // 发送 RTCP 报文（与 RTP 同 Track 同端口复用，RFC 5761）
    // track 未就绪时静默丢弃（设计文档错误处理表约定）
    void sendRtcp(const std::vector<uint8_t>& data);

    // 设置 RTCP 接收回调
    // 参数：cb - 回调函数，收到远端 RTCP 复合包时调用
    void onRtcp(RtcpCallback cb);
```

private 区加（`trackCb_` 声明之后）：

```cpp
    // RTCP 数据接收回调
    RtcpCallback rtcpCb_;

    // 统一安装 Track 的消息回调（RTP/RTCP 去复用后分发）
    // createOffer 与 onTrack 两处创建 Track 都走此函数，避免逻辑重复
    void installTrackHandler();
```

- [ ] **Step 2: `transport_manager.cpp` 实现去复用与发送**

2a. 文件头部 include 区加：

```cpp
#include "media/rtcp/rtcp_packet.h"   // isRtcpPacket：RFC 5761 去复用判定
```

2b. `vecToBinary` 之后加文件局部辅助函数：

```cpp
// ============================================================================
// injectRtcpFb - 向 SDP 的 m=video 节注入 rtcp-fb 反馈能力声明
// ============================================================================
// 【为什么用字符串后处理而非 libdatachannel API】
// libdatachannel 的 Description::Media 未公开按 payload type 添加
// a=rtcp-fb 的接口（兜底方案，设计文档 3.5 节）。
// 本项目两端都是 CrystalRTC，PT=96 为双方约定值，此属性为声明性信息：
// 我方 NACK/PLI 的收发不依赖 SDP 协商结果，但完整的 SDP 有利于
// 将来与浏览器互操作（浏览器只对 SDP 声明了 rtcp-fb 的 PT 发 NACK）。
// 幂等：已包含 rtcp-fb 则原样返回。
static std::string injectRtcpFb(const std::string& sdp) {
    if (sdp.find("a=rtcp-fb:96") != std::string::npos) {
        return sdp;  // 已注入过（createOffer/createAnswer 都会走到这里）
    }
    auto mpos = sdp.find("m=video");
    if (mpos == std::string::npos) return sdp;
    auto lineEnd = sdp.find('\n', mpos);
    if (lineEnd == std::string::npos) return sdp;
    // 插到 m=video 行之后：nack（通用 NACK）+ nack pli（PLI 经由 PSFB）
    const std::string fb = "a=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\n";
    return sdp.insert(lineEnd + 1, fb);
}
```

2c. `PeerConnection::createOffer()` 中，`track_ = track;` 后原来的 `track_->onMessage(...)` 整段 lambda 注册替换为：

```cpp
        track_ = track;
        // 安装去复用消息回调（RTP → onTrack / RTCP → onRtcp）
        installTrackHandler();
```

返回处替换：

```cpp
    auto desc = pc_->localDescription();
    if (desc) {
        // 注入 rtcp-fb 能力声明后再返回给信令
        return injectRtcpFb(std::string(*desc));
    }
    return "";
```

2d. `createAnswer()` 返回处同样替换为 `return injectRtcpFb(std::string(*desc));`

2e. `setRemoteDescription()` 的 `pc_->onTrack` lambda 内，`track_ = track;` 后的 onMessage 注册段替换为：

```cpp
            track_ = track;
            installTrackHandler();  // 被叫路径：远端 Track 到达同样装回调
```

2f. 文件后部（`onTrack()` setter 之前）加两个实现：

```cpp
// ============================================================================
// PeerConnection::installTrackHandler() - 安装 Track 消息回调（去复用）
// ============================================================================
// RFC 5761 RTP/RTCP 复用区分规则（勘误版，见计划头部说明）：
// 看第 2 个字节——RTCP 包类型 ∈ [192,223]；RTP 的 M+PT 组合
// （本项目 PT=96/97，最大 225）落在区间外，判定无歧义。
void PeerConnection::installTrackHandler() {
    track_->onMessage([this](const rtc::binary& data) {
        auto bytes = binaryToVec(data);
        if (isRtcpPacket(bytes.data(), bytes.size())) {
            // RTCP 复合包 → 独立通道交给上层（NACK/PLI/SR/RR 处理）
            if (rtcpCb_) rtcpCb_(bytes);
        } else if (trackCb_) {
            // RTP 包 → 原有媒体链路不变
            trackCb_(bytes);
        }
    }, nullptr);
}

// ============================================================================
// PeerConnection::sendRtcp() - 发送 RTCP 报文
// ============================================================================
// RTCP 与 RTP 走同一条 Track：libdatachannel 把它当作不透明字节经 SRTP
// 发出（RTCP 也受 SRTP 保护），对端用同款去复用逻辑识别。
void PeerConnection::sendRtcp(const std::vector<uint8_t>& data) {
    if (track_ && track_->isOpen()) {
        track_->send(vecToBinary(data.data(), data.size()));
    }
    // track 未就绪：静默丢弃（连接建立早期 RTCP 定时器可能已到期）
}

// ============================================================================
// PeerConnection::onRtcp() - 设置 RTCP 接收回调
// ============================================================================
void PeerConnection::onRtcp(RtcpCallback cb) {
    rtcpCb_ = std::move(cb);
}
```

- [ ] **Step 3: `src/CMakeLists.txt` 给 crystal_transport 加链接**

```cmake
target_link_libraries(crystal_transport PUBLIC
    crystal_utils datachannel crystal_media_rtcp
)
```

（crystal_media_rtcp 库本体已在 Task 1 创建）

- [ ] **Step 4: 构建验证**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_transport crystal_client -j$(nproc) 2>&1 | tail -3`
Expected: 编译成功，无未定义引用

- [ ] **Step 5: Commit**

```bash
git add src/transport/transport_manager.h src/transport/transport_manager.cpp \
        src/CMakeLists.txt
git commit -m "feat(transport): PeerConnection RTP/RTCP 去复用 + sendRtcp + SDP 注入 rtcp-fb"
```

---

### Task 8: `main_client` 全量接线

**Files:**
- Modify: `main_client.cpp`

接线总览（对照设计文档 3.6 节数据流图）：

```
发送侧:  encoder.onEncoded → packetize → retxBuffer.store + sendReport计数 → sendMedia
         alsaCapture.onAudio → packetize → sendReport计数 → sendMedia（音频不做 NACK）
接收侧:  onTrack(视频) → recvReport计数 → nackReq.onReceived → jb.insert
              └ 新间隙 → nackReq.onMissing → sendNack → sendRtcp
         onRtcp → NACK→retxBuffer补发 | PLI→forceKeyframe | RR→RTT | SR→LSR基准
恢复:    decoder.onError / NACK放弃 → sendPli(500ms节流) → sendRtcp
周期:    主循环驱动 nackReq.tick 重试；每 5s 发 SR(或纯 RR)+SDES；打印统计行
```

- [ ] **Step 1: 头部 include 与工具函数**

include 区（`room/room_manager.h` 之后）加：

```cpp
#include "media/rtcp/rtcp_packet.h"
#include "media/rtcp/retransmission_buffer.h"
#include "media/rtcp/nack_requester.h"
#include "media/rtcp/rtcp_reporter.h"
#include <random>
#include <chrono>
```

`signalHandler` 之后加：

```cpp
// 单调时钟毫秒数（RTCP 组件的统一时间源，避免系统时间跳变影响）
static uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
```

- [ ] **Step 2: 随机 SSRC + RTCP 组件创建**

替换步骤 3 中两个 packetizer 的构造（勘误2：SSRC 随机化在调用侧完成）：

```cpp
    // ---- 步骤3：创建 RTP 打包/解包/抖动缓冲区 ----
    // SSRC 随机生成：Phase 1 两端都写死 0x12345678，RTCP 报告块将无法
    // 区分统计属于哪条流；随机化后每条流全局唯一（RFC 3550 8.1 建议）。
    // 初始序列号同样随机化（RFC 3550 5.1：增大攻击者猜测难度）
    std::random_device rd;
    uint32_t videoSsrc = rd();
    uint32_t audioSsrc = rd();
    crystal::RtpPacketizer videoPacketizer(96, 90000, videoSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF));
    crystal::RtpPacketizer audioPacketizer(97, 48000, audioSsrc,
                                           static_cast<uint16_t>(rd() & 0xFFFF));
```

抖动缓冲区之后加 RTCP 组件：

```cpp
    // ---- 步骤3b：RTCP 反馈组件 ----
    // 发送侧：已发视频包缓存，响应对端 NACK 补发（音频不做 NACK——
    // 重传到达已错过播放时刻，Opus 自带 PLC 丢包隐藏更划算）
    crystal::RetransmissionBuffer retxBuffer;
    // 接收侧：丢失包请求状态机（立即请求 + 33ms 重试 + 3 次放弃）
    crystal::NackRequester nackRequester;
    // 统计：发送侧构造 SR / 接收侧构造 RR 报告块（每流一对）
    crystal::SendSideReporter videoSendReport(videoSsrc, "crystal-video");
    crystal::SendSideReporter audioSendReport(audioSsrc, "crystal-audio");
    crystal::RecvSideReporter videoRecvReport(90000);  // 收对端视频(PT=96)
    crystal::RecvSideReporter audioRecvReport(48000);  // 收对端音频(PT=97)
    // 本端是否已发出过包（决定周期发 SR 还是纯 RR，RFC 3550 6.4）
    bool videoSent = false, audioSent = false;
    // 当前帧的 RTP 时间戳（修复 Phase 1 全 0 时间戳问题，见 Step 7）
    uint32_t videoRtpTs = 0, audioRtpTs = 0;
```

- [ ] **Step 3: PLI / NACK 发送辅助 lambda**

回调注册区开头（"回调1"之前）加：

```cpp
    // ---- 辅助：发 PLI（500ms 节流）----
    // 关键帧体积是 P 帧数倍，无节流会造成"错误→PLI→大帧→更易丢→错误"
    // 的正反馈风暴，挤占正常媒体带宽
    uint64_t lastPliSentMs = 0;
    auto sendPli = [&]() {
        if (!videoRecvReport.active()) return;   // 还不知道对端视频 SSRC
        uint64_t now = nowMs();
        if (now - lastPliSentMs < 500) return;   // 节流
        lastPliSentMs = now;
        crystal::PliPacket pli;
        pli.senderSsrc = videoSsrc;              // 发起方（本端）SSRC
        pli.mediaSsrc = videoRecvReport.remoteSsrc();  // 被请求的媒体流
        std::vector<uint8_t> buf;
        crystal::appendPli(buf, pli);
        pc->sendRtcp(buf);
        crystal::Logger::info("RTCP: PLI sent (keyframe requested)");
    };

    // ---- 辅助：发 NACK（seq 列表 → PID+BLP）----
    auto sendNack = [&](const std::vector<uint16_t>& seqs) {
        if (seqs.empty() || !videoRecvReport.active()) return;
        crystal::NackPacket nack;
        nack.senderSsrc = videoSsrc;
        nack.mediaSsrc = videoRecvReport.remoteSsrc();
        nack.entries = crystal::NackPacket::buildEntries(seqs);
        std::vector<uint8_t> buf;
        crystal::appendNack(buf, nack);
        pc->sendRtcp(buf);
    };
```

- [ ] **Step 4: 改写 `pc->onTrack`（接收统计 + NACK 检测）**

视频分支替换为（音频分支仅加一行统计，见后）：

```cpp
    pc->onTrack([&](const std::vector<uint8_t>& data) {
        crystal::RtpPacket pkt;
        if (!pkt.parse(data.data(), data.size())) return;

        if (pkt.payloadType() == 96) {
            // === 接收统计（RR 数据源）：丢包/回绕/抖动 ===
            videoRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            // === NACK 状态机联动 ===
            // 任意到达的包（含重传包）都解除对应 seq 的待请求状态
            nackRequester.onReceived(pkt.sequenceNumber());
            // insert 返回本次新检测到的丢失 seq（≤64 的小间隙才有值）
            auto missing = videoJitterBuf.insert(pkt);
            // 状态机过滤出"现在就该请求"的 seq（其余进入重试队列）
            auto toRequest = nackRequester.onMissing(missing, nowMs());
            sendNack(toRequest);

            auto packets = videoJitterBuf.consume();
            for (const auto& p : packets) {
                auto nals = depacketizer.depacketizeH264(p);
                for (const auto& nal : nals) {
                    decoder.decode(nal.data(), nal.size());
                }
            }
        } else if (pkt.payloadType() == 97) {
            // 音频只做统计（不做 NACK，理由见 Step 2 注释）
            audioRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            auto packets = audioJitterBuf.consume();
            for (const auto& p : packets) {
                auto frames = depacketizer.depacketizeOpus(p);
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());
                    }
                }
            }
        }
    });
```

- [ ] **Step 5: 新增 `pc->onRtcp`（NACK 补发 / PLI / RR / SR）**

`pc->onTrack` 注册之后加：

```cpp
    // ---- 回调1b：接收远端 RTCP 复合包（Task 7 去复用后到达这里）----
    pc->onRtcp([&](const std::vector<uint8_t>& data) {
        crystal::parseRtcpCompound(data.data(), data.size(),
                                   [&](const crystal::RtcpPacket& p) {
            switch (p.kind) {
            case crystal::RtcpKind::Nack:
                // 对端请求重传：展开 PID+BLP 为 seq，逐个从重传缓冲补发
                for (uint16_t seq : crystal::NackPacket::expandEntries(p.nack.entries)) {
                    auto original = retxBuffer.get(seq, nowMs());
                    if (!original.empty()) {
                        pc->sendMedia(original);  // 原样补发缓存 bytes
                    }
                }
                break;
            case crystal::RtcpKind::Pli:
                // 对端画面不可恢复，下一帧强制 IDR
                encoder.forceKeyframe();
                crystal::Logger::info("RTCP: PLI received, forcing keyframe");
                break;
            case crystal::RtcpKind::ReceiverReport: {
                // 对端 RR → 更新我方两条发送流的 RTT/对端观测丢包率
                // （reporter 内部按 SSRC 匹配，各取所需）
                uint64_t ntp = crystal::nowNtp();
                videoSendReport.onReceiverReport(p.rr, ntp);
                audioSendReport.onReceiverReport(p.rr, ntp);
                break;
            }
            case crystal::RtcpKind::SenderReport: {
                // 对端 SR → 记录到达时刻（我方 RR 报告块 LSR/DLSR 基准）
                uint64_t ntp = crystal::nowNtp();
                videoRecvReport.onSenderReport(p.sr.ssrc, ntp);
                audioRecvReport.onSenderReport(p.sr.ssrc, ntp);
                break;
            }
            default:
                break;  // SDES 等本阶段不处理
            }
        });
    });
```

- [ ] **Step 6: 解码错误 → PLI**

回调3（`decoder.onDecoded`）之后加：

```cpp
    // ---- 回调3b：解码错误 → 节流后请求关键帧 ----
    // sendPli 内部 500ms 节流；错误隐藏帧（花屏）同样触发
    decoder.onError([&]() { sendPli(); });
```

- [ ] **Step 7: 发送链路改造（重传缓冲 + 统计 + 时间戳修复）**

7a. 回调4（`encoder.onEncoded`）替换为：

```cpp
    // ---- 回调4：视频编码输出 → RTP 打包 → 发送 ----
    // 同一帧的多个 NAL 共享同一 RTP 时间戳（videoRtpTs 在回调5按帧推进）
    encoder.onEncoded([&](const uint8_t* nalData, size_t nalLen) {
        std::vector<uint8_t> nal(nalData, nalData + nalLen);
        auto packets = videoPacketizer.packetizeH264(nal, videoRtpTs);
        for (const auto& pkt : packets) {
            auto data = pkt.serialize();
            // ① 存入重传缓冲（对端 NACK 时按 seq 补发的就是这份 bytes）
            retxBuffer.store(pkt.sequenceNumber(), data, nowMs());
            // ② 发送侧统计（SR 的包数/字节数/最新时间戳）
            videoSendReport.onPacketSent(pkt.sequenceNumber(),
                                         pkt.payload().size(), pkt.timestamp());
            // ③ 实际发送
            pc->sendMedia(data);
            videoSent = true;
        }
    });
```

7b. 回调5（`capture.onFrame`）替换为：

```cpp
    // ---- 回调5：视频采集 → 编码 ----
    // 【RTP 时间戳修复】Phase 1 所有包 timestamp=0，接收端无法判断帧边界，
    // 也会破坏抖动计算。视频时钟 90kHz，每帧推进 90000/fps。
    capture.onFrame([&](const uint8_t* yuvData, size_t len) {
        videoRtpTs += 90000 / encConfig.fps;  // 一帧周期（30fps → 3000）
        encoder.encode(yuvData, len);         // encode 同步触发回调4
    });
```

7c. 回调6（`alsaCapture.onAudio`）替换为：

```cpp
    // ---- 回调6：音频采集 → 编码 → RTP 打包 → 发送 ----
    // 音频时钟 48kHz，每帧推进 = 每帧采样数（Opus @48kHz）
    alsaCapture.onAudio([&](const int16_t* data, size_t samples) {
        auto opusFrame = opusEncoder.encode(data, opusEncoder.frameSize());
        if (!opusFrame.empty()) {
            audioRtpTs += static_cast<uint32_t>(opusEncoder.frameSize());
            auto packets = audioPacketizer.packetizeOpus(opusFrame, audioRtpTs);
            for (const auto& pkt : packets) {
                auto data = pkt.serialize();
                audioSendReport.onPacketSent(pkt.sequenceNumber(),
                                             pkt.payload().size(), pkt.timestamp());
                pc->sendMedia(data);
                audioSent = true;
            }
        }
    });
```

- [ ] **Step 8: 主循环改造（NACK 重试 + SR/RR 周期 + 统计打印）**

主循环 `while (g_running ...)` 替换为：

```cpp
    // ====================================================================
    // 步骤10：主循环
    // 轮询 SDL 事件 + 驱动 RTCP 周期任务（NACK 重试 / SR-RR / 统计）
    // ====================================================================
    uint64_t lastReportMs = 0;
    uint64_t lastGivenUp = 0;
    while (g_running && !renderer.shouldQuit()) {
        renderer.pollEvents();
        uint64_t now = nowMs();

        // --- NACK 重试驱动：到期未恢复的 seq 再次请求 ---
        sendNack(nackRequester.tick(now));

        // --- 重试耗尽 → PLI 兜底（参考帧链已断，重传救不回来）---
        if (nackRequester.givenUpCount() > lastGivenUp) {
            lastGivenUp = nackRequester.givenUpCount();
            sendPli();
        }

        // --- 每 5s：SR/RR + 统计行（RFC 3550 推荐周期）---
        if (now - lastReportMs >= 5000) {
            lastReportMs = now;
            uint64_t ntp = crystal::nowNtp();

            // 视频流：发过包 → SR(内嵌对端接收报告块)；只收未发 → 纯 RR
            crystal::ReportBlock blk;
            std::vector<crystal::ReportBlock> blocks;
            if (videoRecvReport.buildBlock(ntp, blk)) blocks.push_back(blk);
            if (videoSent) {
                pc->sendRtcp(videoSendReport.buildReport(ntp, blocks));
            } else if (!blocks.empty()) {
                crystal::ReceiverReport rr;
                rr.ssrc = videoSsrc;      // 报告发起方 SSRC
                rr.blocks = blocks;
                std::vector<uint8_t> buf;
                crystal::appendReceiverReport(buf, rr);
                crystal::SdesPacket sdes;
                sdes.ssrc = videoSsrc;
                sdes.cname = "crystal-video";
                crystal::appendSdes(buf, sdes);   // 复合包需含 SDES
                pc->sendRtcp(buf);
            }
            // 音频流：同理
            blocks.clear();
            if (audioRecvReport.buildBlock(ntp, blk)) blocks.push_back(blk);
            if (audioSent) {
                pc->sendRtcp(audioSendReport.buildReport(ntp, blocks));
            } else if (!blocks.empty()) {
                crystal::ReceiverReport rr;
                rr.ssrc = audioSsrc;
                rr.blocks = blocks;
                std::vector<uint8_t> buf;
                crystal::appendReceiverReport(buf, rr);
                crystal::SdesPacket sdes;
                sdes.ssrc = audioSsrc;
                sdes.cname = "crystal-audio";
                crystal::appendSdes(buf, sdes);
                pc->sendRtcp(buf);
            }

            // --- 终端质量统计行 ---
            std::string rtt = videoSendReport.hasRtt()
                                  ? std::to_string(
                                        static_cast<int>(videoSendReport.rttMs()))
                                  : "n/a";
            crystal::Logger::info(
                "[stats] 丢包 v{:.1f}% a{:.1f}% | 抖动 v{:.1f}ms a{:.1f}ms | "
                "RTT {}ms | 重传 {} miss {} | NACK {} 放弃 {}",
                videoRecvReport.lossRate() * 100, audioRecvReport.lossRate() * 100,
                videoRecvReport.jitterMs(), audioRecvReport.jitterMs(), rtt,
                retxBuffer.retransmittedCount(), retxBuffer.missCount(),
                nackRequester.requestedCount(), nackRequester.givenUpCount());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
```

- [ ] **Step 9: 全量构建 + 全部测试**

Run: `cd /workspace/build && cmake .. > /dev/null && make crystal_client crystal_rtcp_tests crystal_rtp_tests -j$(nproc) 2>&1 | tail -3 && ./crystal_rtcp_tests && ./crystal_rtp_tests`
Expected: 编译成功；两套测试全绿

- [ ] **Step 10: Commit**

```bash
git add main_client.cpp
git commit -m "feat(client): RTCP 全量接线——NACK/PLI/SR-RR 收发、统计打印、RTP 时间戳修复"
```

---

### Task 9: 文档同步与设计文档勘误回写

**Files:**
- Modify: `README.md`
- Modify: `docs/USAGE.md`
- Modify: `docs/LEARNING_GUIDE.md`
- Modify: `docs/superpowers/specs/2026-08-17-rtcp-feedback-design.md`

- [ ] **Step 1: `README.md` 特性表**

在已有特性列表（视频/音频/P2P 各条目之后）加：

```markdown
- **RTCP 反馈体系（全自实现，RFC 3550/4585/5761）**
  - NACK 丢包重传：JitterBuffer 间隙检测 → 状态机请求/重试/放弃 → 发送端重传缓冲补发
  - PLI 关键帧请求：解码错误/NACK 放弃触发（500ms 节流），编码器强制 IDR
  - SR/RR 质量统计：丢包率、到达间隔抖动（RFC 3550 A.8）、RTT（LSR/DLSR 法 A.6），终端周期打印
  - RTP/RTCP 单端口复用去复用（RFC 5761）
```

- [ ] **Step 2: `docs/USAGE.md` 新增两节**

2a. 「质量统计日志说明」节（放在运行说明之后）：

````markdown
## 质量统计日志

连接建立后每 5 秒打印一行质量统计：

```
[stats] 丢包 v2.3% a0.0% | 抖动 v8.5ms a1.2ms | RTT 45ms | 重传 12 miss 2 | NACK 15 放弃 1
```

| 字段 | 含义 | 数据来源 |
|------|------|----------|
| 丢包 v/a | 对端视频/音频流的累计丢包率 | 本端 RecvSideReporter（RR 同源数据） |
| 抖动 v/a | 到达间隔平滑抖动（RFC 3550 A.8 指数平滑） | 本端 RecvSideReporter |
| RTT | 网络往返延迟（LSR/DLSR 法，RFC 3550 A.6） | 本端收到对端 RR 时计算 |
| 重传 / miss | 本端响应 NACK 补发次数 / 缓冲未命中次数 | RetransmissionBuffer |
| NACK / 放弃 | 本端发出的重传请求数 / 重试 3 次后放弃数 | NackRequester |
````

2b. 「弱网测试（tc netem）」节：

````markdown
## 弱网测试（tc netem）

在**接收端**主机的出口网卡上注入丢包，验证 NACK/PLI 生效：

```bash
# 10% 丢包 + 20ms 延迟
sudo tc qdisc add dev <网卡名> root netem delay 20ms loss 10%

# 恢复
sudo tc qdisc del dev <网卡名> root
```

观察要点：
1. `[stats]` 行丢包率明显低于 netem 注入值 → NACK 重传在补包
2. 丢包率极低但出现花屏时约 0.5-1s 内恢复 → PLI 关键帧请求生效
3. `NACK 放弃` 计数增长伴随日志 `PLI sent` → 重试耗尽后 PLI 兜底
4. 对照实验：`loss 30%` 以上时 NACK 收益下降（重传也丢），主要靠 PLI 恢复
````

- [ ] **Step 3: `docs/LEARNING_GUIDE.md` 新增 Module 8**

按现有 Module 的体例（概念讲解 → 代码走读 → 动手练习 → 面试高频题）新增：

````markdown
# Module 8: RTCP 反馈体系与弱网对抗

## 8.1 核心概念
- RTCP 与 RTP 的关系：同端口复用（RFC 5761），控制与数据分离
- 五种报文：SR/RR/SDES/NACK/PLI 的字段结构与用途
- RTT 计算三要素：LSR + DLSR + 到达时刻（为什么只能在发送侧算）
- 抖动的指数平滑：`jitter += (D - jitter) / 16` 的直觉（1/16 增益的权衡）
- NACK 的 PID+BLP 位图：4 字节表达 17 个丢失包的压缩技巧
- NACK vs PLI vs FIR 的适用场景（单包丢失 / 参考链断裂 / 解码器彻底崩）
- 为什么音频不做 NACK（延迟预算 + Opus PLC）而视频做

## 8.2 代码走读
| 组件 | 文件 | 重点 |
|------|------|------|
| 协议层 | src/media/rtcp/rtcp_packet.cpp | 字节级序列化/解析、复合包迭代 |
| 重传缓冲 | src/media/rtcp/retransmission_buffer.cpp | TTL+容量双淘汰、环形语义 |
| NACK 状态机 | src/media/rtcp/nack_requester.cpp | onMissing/tick/onReceived 协作 |
| 统计器 | src/media/rtcp/rtcp_reporter.cpp | RFC A.1/A.6/A.8 三个算法 |
| 去复用 | src/transport/transport_manager.cpp installTrackHandler | RFC 5761 第二字节判据 |
| 接线 | main_client.cpp | 收发两侧完整数据流 |

## 8.3 动手练习
1. 把 NACK 重试间隔 33ms 改成 100ms，弱网下丢包率/延迟如何变化？
2. 关掉 PLI 节流（500ms→0），观察关键帧风暴对带宽统计的影响
3. 用 tcpdump 抓包，对照 RFC 4585 手工解码一个 NACK 报文

## 8.4 面试高频题
- "WebRTC 怎么处理丢包？"（三层答案：jitter buffer 吸收乱序、NACK 要重传、PLI 换关键帧；再提 FEC/带宽估计引申）
- "RTT 怎么测的？为什么不用 ping？"（RTCP LSR/DLSR 与媒体流同路径、穿越同 NAT）
- "NACK 和 PLI 什么时候选谁？"
- "RTCP 报文会加密吗？"（SRTP 下 SRTCP 保护，本项目经 libdatachannel track 发送即受保护）
````

- [ ] **Step 4: 设计文档勘误回写**

`docs/superpowers/specs/2026-08-17-rtcp-feedback-design.md`：

4a. 3.5 节表格 `transport_manager` 行的去复用描述改为：

> RTP/RTCP 按第二字节（RTCP 包类型 ∈ [192,223]）判定（RFC 5761 §4 实际判据，勘误：原文"首字节低 7 位"有误）

4b. 3.5 节 `rtp_packetizer.*` 行改为：

> ~~SSRC 由写死值改为随机生成~~ **勘误：无需改 packetizer 本体**——构造函数已支持 `ssrc/startSeqNum` 参数，随机化在 `main_client.cpp` 调用侧完成

4c. 3.5 节 SDP 行改为：

> SDP 注入采用字符串后处理方案（libdatachannel v0.21.2 无按 PT 添加 rtcp-fb 的公开 API，实勘查明）；注入位置为 m=video 行后，幂等

- [ ] **Step 5: Commit**

```bash
git add README.md docs/USAGE.md docs/LEARNING_GUIDE.md \
        docs/superpowers/specs/2026-08-17-rtcp-feedback-design.md
git commit -m "docs: RTCP 反馈体系文档同步（指南 Module 8 / 弱网测试 / 特性表 / 设计勘误）"
```

---

## 验收清单（全部任务完成后）

- [ ] `./crystal_rtcp_tests` 全绿（Task 1-5 新增用例）
- [ ] `./crystal_rtp_tests` 17+ 存量用例全绿（无回归）
- [ ] `crystal_client` 编译通过
- [ ] 两客户端互联（同机双实例）时每 5s 出现 `[stats]` 行
- [ ] `tc netem loss 10%` 下画面 1s 内可恢复，丢包统计低于注入值

