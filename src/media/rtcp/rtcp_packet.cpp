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
// 【为什么需要这些函数】RTCP 规定所有多字节字段用网络字节序（大端）：
// 高位字节在前。而 x86/ARM CPU 内部是小端，直接 memcpy 内存会字节颠倒，
// 所以每个字段都要"手动"按大端逐字节写入/读出。
//
// 【大端图示：写入 16 进制值 0xABCD】
//   内存布局: [0xAB] [0xCD]   ← 高字节在低地址（先发到网络）
//   putU16 就是先 push 0xAB 再 push 0xCD；getU16 反过来拼装。
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

// 解析 24 字节报告块（字段偏移表，见下方 ReportBlock::serialize 的图示）
bool parseReportBlock(const uint8_t* p, ReportBlock& blk) {
    // 偏移 0-3:   SSRC（被统计的流是谁）
    blk.ssrc = getU32(p);
    // 偏移 4:     间隔丢包率 fraction lost（单字节，256=100%）
    blk.fractionLost = p[4];
    // 偏移 5-7:   累计丢包数（24 位，所以用三个字节手动拼）
    blk.cumulativeLost = (static_cast<uint32_t>(p[5]) << 16) |
                         (static_cast<uint32_t>(p[6]) << 8) |
                         static_cast<uint32_t>(p[7]);
    // 偏移 8-11:  扩展最高序列号（含回绕计数）
    blk.extHighestSeq = getU32(p + 8);
    // 偏移 12-15: 到达间隔抖动
    blk.jitter = getU32(p + 12);
    // 偏移 16-19: LSR（对端最近 SR 的 NTP 中间 32 位）
    blk.lsr = getU32(p + 16);
    // 偏移 20-23: DLSR（对端收 SR 到发 RR 的延迟）
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
//
// 【报告块的 24 字节内存布局（RFC 3550 6.4.1）】
//
//   偏移:  0    4      5       8          12      16    20
//        ┌────┬────┬───────┬─────────┬───────┬─────┬─────┐
//        │SSRC│frac│ cumul │extMaxSeq│jitter │ LSR │DLSR │
//        │ 4B │ 1B │ lost 3B│   4B   │  4B   │ 4B  │ 4B  │
//        └────┴────┴───────┴─────────┴───────┴─────┴─────┘
//
// 【数值示例】丢包率 5% 的 fraction 值 = 5% × 256 ≈ 13（0x0D）
void ReportBlock::serialize(std::vector<uint8_t>& out) const {
    putU32(out, ssrc);
    out.push_back(fractionLost);
    // cumulativeLost 只有 24 位：& 0xFFFFFF 截掉高位，防溢出协议字段
    putU24(out, cumulativeLost & 0xFFFFFF);
    putU32(out, extHighestSeq);
    putU32(out, jitter);
    putU32(out, lsr);
    putU32(out, dlsr);
}

// ============================================================================
// 构造器
// ============================================================================
// 四个 append 函数套路一致：先拼 body（头部之后的字段），再写 4 字节头。
//
// 【为什么先拼 body 再写头】头部里的 length 字段 = 总字节数/4 - 1，
// 必须先知道 body 有多长才能算——所以 body 在前，头压在后插到 out 末尾。
//
// 【length 字段的换算（以 1 个报告块的 SR 为例）】
//   总长 = 4(头) + 24(SR主体) + 24(报告块) = 52 字节
//   length = 52/4 - 1 = 12   ← 单位是"字"（4 字节），不含自身减 1
//   代码里写 body.size()/4：body=48 字节，48/4=12，与上式一致（数学恒等）
void appendSenderReport(std::vector<uint8_t>& out, const SenderReport& sr) {
    // 先构造头部之后的 body，便于计算 length 字段
    //
    // 【SR 的 body 布局】
    //   SSRC(4) | NTP时间戳(8) | RTP时间戳(4) | 包数(4) | 字节数(4) | 报告块×n(24×n)
    //   NTP 64 位拆成高/低两个 32 位分别写入（putU32 一次只能写 4 字节）
    std::vector<uint8_t> body;
    putU32(body, sr.ssrc);
    putU32(body, static_cast<uint32_t>(sr.ntpTimestamp >> 32));      // NTP 高 32 位（秒）
    putU32(body, static_cast<uint32_t>(sr.ntpTimestamp & 0xFFFFFFFF)); // NTP 低 32 位（小数）
    putU32(body, sr.rtpTimestamp);
    putU32(body, sr.packetCount);
    putU32(body, sr.octetCount);
    for (const auto& blk : sr.blocks) blk.serialize(body);

    // 字节0: V=2(2bit) | P=0(1bit) | RC=报告块数(5bit)
    // 0x80 = 二进制 10 0 00000（V=2, P=0），再或上块数
    out.push_back(static_cast<uint8_t>(0x80 | (sr.blocks.size() & 0x1F)));
    out.push_back(RTCP_PT_SR);                                   // 字节1: PT=200
    putU16(out, static_cast<uint16_t>(body.size() / 4));  // 字节2-3: length = (4+body)/4 - 1
    out.insert(out.end(), body.begin(), body.end());
}

void appendReceiverReport(std::vector<uint8_t>& out, const ReceiverReport& rr) {
    // 【RR 的 body 布局】SSRC(4) | 报告块×n(24×n)
    // RR 就是"没有发送统计部分的 SR"，其余字段语义完全一致
    std::vector<uint8_t> body;
    putU32(body, rr.ssrc);
    for (const auto& blk : rr.blocks) blk.serialize(body);

    out.push_back(static_cast<uint8_t>(0x80 | (rr.blocks.size() & 0x1F)));
    out.push_back(RTCP_PT_RR);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendSdes(std::vector<uint8_t>& out, const SdesPacket& sdes) {
    // 【SDES chunk 的 body 布局】SSRC(4) | type=1(1) | len(1) | CNAME 字符 | null 填充
    std::vector<uint8_t> body;
    putU32(body, sdes.ssrc);
    body.push_back(1);  // 类型 1 = CNAME（规范要求每个复合包必带）
    body.push_back(static_cast<uint8_t>(sdes.cname.size()));
    body.insert(body.end(), sdes.cname.begin(), sdes.cname.end());
    // chunk 以 null 填充至 32 位对齐（RFC 3550 6.5）
    // 【为什么必须 4 字节对齐】RTCP 的 length 字段以 4 字节为单位，
    // 不对齐的话整个子包的长度就不再是 4 的倍数，对端解析会错位
    while (body.size() % 4 != 0) body.push_back(0);

    out.push_back(0x80 | 0x01);  // RC=1：1 个 chunk
    out.push_back(RTCP_PT_SDES);
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendNack(std::vector<uint8_t>& out, const NackPacket& nack) {
    // 【NACK 的 body 布局】发送方SSRC(4) | 媒体流SSRC(4) | FCI条目×n(4×n)
    // 每个 FCI 条目: PID(2) + BLP(2)，见下方 buildEntries 的详细图示
    std::vector<uint8_t> body;
    putU32(body, nack.senderSsrc);
    putU32(body, nack.mediaSsrc);
    for (const auto& e : nack.entries) {
        putU16(body, e.pid);
        putU16(body, e.blp);
    }

    // 字节0 的低 5 位对反馈包而言是 FMT（不是 SR/RR 的 RC）：FMT=1 即 NACK
    out.push_back(0x80 | RTCP_FMT_NACK);  // FMT=1
    out.push_back(RTCP_PT_RTPFB);         // PT=205（传输层反馈）
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

void appendPli(std::vector<uint8_t>& out, const PliPacket& pli) {
    // 【PLI 的 body 布局】发送方SSRC(4) | 媒体流SSRC(4)，仅此 8 字节
    // 不需要携带更多信息——"给我关键帧"一句话就够了
    std::vector<uint8_t> body;
    putU32(body, pli.senderSsrc);
    putU32(body, pli.mediaSsrc);

    out.push_back(0x80 | RTCP_FMT_PLI);  // FMT=1
    out.push_back(RTCP_PT_PSFB);         // PT=206（负载层反馈）
    putU16(out, static_cast<uint16_t>(body.size() / 4));
    out.insert(out.end(), body.begin(), body.end());
}

// ============================================================================
// NACK PID+BLP 合并与展开
// ============================================================================
//
// 【PID+BLP 是 RTCP 的"压缩黑科技"】
//   FCI 条目 = PID(16bit) + BLP(16bit)：
//     PID —— 丢失包中最早（最小）的序列号
//     BLP —— 位图，第 i 位（i=0..15）为 1 表示序列号 PID+i+1 也丢了
//   一个 4 字节条目最多表达 17 个连续丢失包（PID 本身 + 16 个位图位）。
//
// 【完整示例】丢失 {100, 102, 103, 200}
//   条目1: PID=100, BLP=0b00000110=0x0006
//          位 1 置 1 → 102 丢；位 2 置 1 → 103 丢（101 没丢所以位 0 是 0）
//   条目2: PID=200, BLP=0x0000（200 距 100 超过 16，位图够不着，只能单开）
//   4 个 seq 从 8 字节（4×2）压到 8 字节（2×4）——丢失越连续压缩率越高。
//
// 【回绕场景】丢失 {65534, 65535, 0, 1}
//   以首元素 65534 为基准，(seq - 65534) mod 65536 依次为 0,1,2,3，
//   可合并为一个条目 PID=65534, BLP=0b00001110。
std::vector<NackEntry> NackPacket::buildEntries(std::vector<uint16_t> lostSeqs) {
    if (lostSeqs.empty()) return {};
    // 以首元素为基准的模 65536 排序：正确处理回绕
    // （数值排序会把 {65534,65535,0,1} 拆散——0/1 排到最前，无法合并位图）
    const uint16_t base = lostSeqs.front();
    std::sort(lostSeqs.begin(), lostSeqs.end(),
              [base](uint16_t a, uint16_t b) {
                  // uint16_t 减法天然是模 65536：65534-65534=0，0-65534=2
                  // 直觉上"负数"变成了小正数，回绕序列因此恢复单调
                  return static_cast<uint16_t>(a - base) <
                         static_cast<uint16_t>(b - base);
              });
    // 排完序后重复值必然相邻，unique 一步去重
    lostSeqs.erase(std::unique(lostSeqs.begin(), lostSeqs.end()), lostSeqs.end());

    // 贪心合并：顺序扫描，塞得进当前条目位图就塞，塞不下开新条目
    std::vector<NackEntry> out;
    for (uint16_t s : lostSeqs) {
        if (out.empty() || static_cast<uint16_t>(s - out.back().pid) > 16) {
            // 距上一个 PID 超过 16 → 位图只有 16 位，覆盖不到，开新条目
            out.push_back({s, 0});
        } else {
            // BLP 第 (s-pid-1) 位置 1，表示 PID+i+1 丢失
            // 例：pid=100, s=102 → 置位 1（102 = 100 + 1 + 1）
            out.back().blp |= static_cast<uint16_t>(1u << (s - out.back().pid - 1));
        }
    }
    return out;
}

// buildEntries 的逆运算：解析对端 NACK 时把条目还原成 seq 列表
// （测试也用它做 round-trip 校验：build 再 expand 应还原出原集合）
std::vector<uint16_t> NackPacket::expandEntries(const std::vector<NackEntry>& entries) {
    std::vector<uint16_t> out;
    for (const auto& e : entries) {
        out.push_back(e.pid);  // PID 本身一定丢
        // 逐位检查 BLP：第 i 位为 1 → seq = PID + i + 1 也丢
        // 加法回绕由 uint16_t 溢出语义自动处理（65535 + 1 = 0）
        for (int i = 0; i < 16; i++) {
            if (e.blp & (1u << i)) out.push_back(static_cast<uint16_t>(e.pid + i + 1));
        }
    }
    return out;
}

// ============================================================================
// 复合包解析
// ============================================================================
//
// 【什么是复合包】一次 UDP 载荷里可以背靠背拼多个 RTCP 子包，
// 例如本项目周期性发出的就是 [SR + SDES] 两个子包的复合包。
//
// 【解析循环的走法（示例：SR+SDES 复合包）】
//   offset=0:  读头部 → PT=200(SR)，length=12 → 本子包共 52 字节
//              → 解析 SR 字段，回调 handler → offset 前进到 52
//   offset=52: 读头部 → PT=202(SDES)，length=5 → 本子包共 24 字节
//              → 解析 CNAME，回调 handler → offset 前进到 76
//   offset=76: 76 == size → 循环结束，返回 true（完整消费）
//
// 【防御性设计】任何一步校验失败（版本错/长度越界）立即返回 false，
// 宁可丢弃整个包也不带病解析——畸形包多半是网络损坏或恶意构造。
bool parseRtcpCompound(const uint8_t* data, size_t size,
                       const RtcpPacketHandler& handler) {
    size_t offset = 0;
    // 条件 offset + 4 <= size：至少要剩 4 字节才够读一个 RTCP 头部
    while (offset + 4 <= size) {
        const uint8_t* p = data + offset;

        // 版本号必须为 2（字节 0 的高 2 位；RTP/RTCP 当前唯一版本）
        if ((p[0] >> 6) != 2) return false;

        // 字节 0 低 5 位：SR/RR 里叫 RC（报告块数），反馈包里叫 FMT（子类型）
        // 同一个位置两种含义，所以这里取个中性名字 fmtOrCount
        uint8_t fmtOrCount = p[0] & 0x1F;
        uint8_t pt = p[1];
        // length 单位是 4 字节且"不含自身减 1"，所以真实字节数 = (length+1)*4
        size_t byteLen = (static_cast<size_t>(getU16(p + 2)) + 1) * 4;

        // length 字段声明的长度超过剩余数据 → 畸形，停止
        if (offset + byteLen > size) return false;

        // body = 头部之后的字段区（长度已在上面通过 byteLen 校验过）
        size_t bodyLen = byteLen - 4;
        const uint8_t* body = p + 4;

        RtcpPacket pkt;
        switch (pt) {
        case RTCP_PT_SR: {
            // SR 固定部分 24 字节：SSRC(4)+NTP(8)+RTP时间戳(4)+包数(4)+字节数(4)
            if (bodyLen < 24) return false;
            pkt.kind = RtcpKind::SenderReport;
            pkt.sr.ssrc = getU32(body);
            // 两个 32 位拼回 64 位 NTP 时间戳：高 32 位左移 32 位再或上低 32 位
            pkt.sr.ntpTimestamp =
                (static_cast<uint64_t>(getU32(body + 4)) << 32) | getU32(body + 8);
            pkt.sr.rtpTimestamp = getU32(body + 12);
            pkt.sr.packetCount = getU32(body + 16);
            pkt.sr.octetCount = getU32(body + 20);
            // 报告块数取 RC 与实际剩余空间的最小值（防御畸形包）
            // 头部说有 10 个块但只剩 2 块的空间 → 只解析 2 块，不越界
            size_t n = std::min<size_t>(fmtOrCount, (bodyLen - 24) / 24);
            for (size_t i = 0; i < n; i++) {
                ReportBlock blk;
                parseReportBlock(body + 24 + i * 24, blk);
                pkt.sr.blocks.push_back(blk);
            }
            break;
        }
        case RTCP_PT_RR: {
            // RR 主体只有 SSRC(4)，后面直接跟报告块
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
            // SDES body 是 [SSRC(4)][type(1)][len(1)][文本...] 的项列表，
            // type=0 表示 chunk 结束
            if (bodyLen < 4) return false;
            pkt.kind = RtcpKind::Sdes;
            pkt.sdes.ssrc = getU32(body);
            // 仅解析第一个 chunk 的 CNAME 项（本项目每包一个 chunk）
            size_t pos = 4;
            while (pos + 2 <= bodyLen) {
                uint8_t type = body[pos];
                if (type == 0) break;  // chunk 结束标志（list terminator）
                uint8_t len = body[pos + 1];
                if (pos + 2 + len > bodyLen) break;  // 长度越界，防御
                if (type == 1) {
                    // type=1 即 CNAME，本项目唯一关心的 SDES 项
                    pkt.sdes.cname.assign(reinterpret_cast<const char*>(body + pos + 2), len);
                }
                pos += 2 + len;
            }
            break;
        }
        case RTCP_PT_RTPFB: {
            // PT=205 是大类"传输层反馈"，具体是哪种看 FMT：
            // FMT=1 → NACK；其他值（如通用 NACK 的变体）本项目不识别
            if (fmtOrCount == RTCP_FMT_NACK) {  // 仅识别 NACK
                if (bodyLen < 8) return false;
                pkt.kind = RtcpKind::Nack;
                pkt.nack.senderSsrc = getU32(body);
                pkt.nack.mediaSsrc = getU32(body + 4);
                // 剩余空间全部是 4 字节 FCI 条目（PID+BLP）
                size_t n = (bodyLen - 8) / 4;
                for (size_t i = 0; i < n; i++) {
                    const uint8_t* f = body + 8 + i * 4;
                    pkt.nack.entries.push_back({getU16(f), getU16(f + 2)});
                }
            }
            break;  // 其他 FMT 不识别，跳过该子包
        }
        case RTCP_PT_PSFB: {
            // PT=206 是大类"负载层反馈"，FMT=1 → PLI（关键帧请求）
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

        // 只把识别出的子包交给上层；不认识的静默跳过（向前兼容）
        if (pkt.kind != RtcpKind::Unknown && handler) handler(pkt);
        offset += byteLen;  // 前进到下一个子包
    }
    return offset == size;  // 完整消费才算成功（末尾有残缺字节则返回 false）
}

// ============================================================================
// NTP 时间工具
// ============================================================================
//
// 【NTP 时间戳格式】64 位 = 高 32 位秒（自 1900-01-01 起）+ 低 32 位小数
//   1 秒被分成 2^32 份，精度约 0.23 纳秒——远超媒体需求，绰绰有余
//
// 【为什么不用 Unix 时间戳】RTCP 协议生来用 NTP 纪元（1900），向后兼容
// 老设备。SR/RR 里的 LSR/DLSR 都从它派生，换算关系：
//   NTP 秒 = Unix 秒 + 2208988800（1900→1970 相隔的秒数）
uint64_t nowNtp() {
    using namespace std::chrono;
    // system_clock 从 Unix 纪元（1970）起算，先拆成"整秒 + 纳秒余数"
    auto now = system_clock::now().time_since_epoch();
    auto secs = duration_cast<seconds>(now).count();
    auto nanos = duration_cast<nanoseconds>(now).count() % 1000000000LL;
    // NTP 纪元是 1900，Unix 纪元是 1970，偏移 2208988800 秒
    uint64_t secPart = static_cast<uint64_t>(secs) + 2208988800ULL;
    // 纳秒 → 32 位 NTP 小数部分
    // 例：0.5 秒 = 500000000ns → 500000000 × 2^32 / 10^9 ≈ 0x80000000（正好一半）
    uint64_t fracPart =
        static_cast<uint64_t>(nanos) * (1ULL << 32) / 1000000000ULL;
    return (secPart << 32) | fracPart;
}

// 取 NTP 时间戳的"中间 32 位"：秒的低 16 位 + 小数的高 16 位
// 【为什么掐头去尾】RTCP 报告块的 LSR 字段只有 32 位，掐掉秒的高 16 位
// （约 18 小时才回绕一次）保留精度最高的中间部分，是 RFC 3550 的折中。
// 配合 DLSR 一起使用时，掐掉的部分在减法中相互抵消，不影响 RTT 计算。
uint32_t ntpMiddle32(uint64_t ntp) {
    return static_cast<uint32_t>((ntp >> 16) & 0xFFFFFFFF);
}

} // namespace crystal
