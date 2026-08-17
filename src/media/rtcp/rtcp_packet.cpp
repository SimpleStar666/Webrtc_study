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
    // 第二字节：RTCP 包类型 192-223；RTP 的 M+PT 组合
    // （本项目 PT=96/97，M=1 时第二字节 224/225，M=0 时 96/97，均落在区间外，判定无歧义）
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
    if (lostSeqs.empty()) return {};
    // 以首元素为基准的模 65536 排序：正确处理回绕
    // （数值排序会把 {65534,65535,0,1} 拆散——0/1 排到最前，无法合并位图）
    const uint16_t base = lostSeqs.front();
    std::sort(lostSeqs.begin(), lostSeqs.end(),
              [base](uint16_t a, uint16_t b) {
                  return static_cast<uint16_t>(a - base) <
                         static_cast<uint16_t>(b - base);
              });
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
