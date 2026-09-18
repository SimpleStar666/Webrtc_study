// ============================================================================
// transport_feedback.cpp - TWCC TransportFeedback 编解码实现
// ============================================================================
//
// 【算法思路】
// 编码（接收端）：
//   1. 把 received/lost 样本按 (seq - baseSeq) 模 65536 偏移整理成窗口，
//      中间缺口按"未收到"（symbol 00）补齐——窗口数 = 最大偏移 + 1
//   2. 基准时刻量化到 1/64ms 网格（refTime 24bit）
//   3. 到达时刻差分量化到 250us 网格（delta 2 字节有符号，链式累加）
//   4. 每 7 个 symbol 打包成一个 StatusVectorChunk（0xC0 前缀 = T=1,S=1）
//   5. 尾部按 RTCP 规则补齐 4 字节对齐（P 位置 1，末字节 = padding 数）
//
// 解码（发送端）：逆向。symbol 流展开后，遇 10 取 2 字节有符号 delta、
// 遇 01 取 1 字节无符号 delta（兼容编码）、遇 00/11 记丢失，
// 到达时刻 = refTime + Σdelta×0.25ms。
//
// 【量化误差】
// refTime 网格 1/64ms（≈0.016ms）、delta 网格 0.25ms，均远小于网络抖动，
// 对 Trendline 斜率估计（量级 0.01+）无影响。
// ============================================================================

#include "media/rtcp/transport_feedback.h"
#include "media/rtcp/rtcp_packet.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace crystal {

// ============================================================================
// 内部字节序辅助（与 rtcp_packet.cpp 同款，静态库内不共享符号）
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

// 2-bit symbol 取值（draft-holmer）
constexpr uint8_t kSymbolNotReceived = 0b00;  // 未收到
constexpr uint8_t kSymbolSmallDelta  = 0b01;  // 已收到，1 字节无符号 delta
constexpr uint8_t kSymbolLargeDelta  = 0b10;  // 已收到，2 字节有符号 delta

// status count 字段宽度（15 位，第 16 位是恒 0 的 B 位）
constexpr size_t kMaxStatusCount = 0x7FFF;

} // namespace

// ============================================================================
// 编码：结构化 TwccFeedback → RTCP 子包字节流
// ============================================================================
std::vector<uint8_t> appendTransportFeedback(const TwccFeedback& fb) {
    // ---- 1. 样本按偏移整理（模 65536 算术，回绕安全；无序容忍）----
    // offset = uint16(seq - baseSeq)：窗口内 seq 单调递增（含回绕）
    std::map<size_t, double> recv;  // offset -> arrivalMs（重复 seq 后者胜）
    for (const auto& s : fb.received)
        recv[static_cast<size_t>(static_cast<uint16_t>(s.seq - fb.baseSeq))] =
            s.arrivalMs;

    std::vector<size_t> lostOff;
    for (uint16_t s : fb.lost) {
        size_t off = static_cast<uint16_t>(s - fb.baseSeq);
        if (recv.count(off) == 0) lostOff.push_back(off);  // received 优先
    }
    std::sort(lostOff.begin(), lostOff.end());

    // ---- 2. 窗口大小 = 最大偏移 + 1（缺口按未收到补 symbol）----
    size_t count = 0;
    if (!recv.empty()) count = recv.rbegin()->first + 1;
    if (!lostOff.empty()) count = std::max(count, lostOff.back() + 1);
    count = std::min(count, kMaxStatusCount);  // 15 位上限防御

    std::vector<uint8_t> symbols(count, kSymbolNotReceived);
    for (const auto& kv : recv)
        if (kv.first < count) symbols[kv.first] = kSymbolLargeDelta;

    // ---- 3. 基准时刻量化（1/64ms 网格，24 位无符号）----
    // delta 链以此为原点：解码端 arrival = refTime + Σdelta×0.25
    long rt = std::lround(fb.refTimeMs * 64.0);
    if (rt < 0) rt = 0;
    if (rt > 0xFFFFFF) rt = 0xFFFFFF;
    const double quantRefMs = static_cast<double>(rt) / 64.0;

    // ---- 4. body 构造 ----
    std::vector<uint8_t> body;
    putU32(body, fb.senderSsrc);
    putU32(body, fb.mediaSsrc);
    putU16(body, fb.baseSeq);
    putU16(body, static_cast<uint16_t>((count << 1) | 0));  // statusCount | B=0
    putU24(body, static_cast<uint32_t>(rt));
    body.push_back(0);  // fb packet count（本实现不计数）

    // StatusVectorChunk：每 7 个 symbol 一块，块首 0xC0（T=1 | S=2bit）
    // 不足 7 个的尾部块，空位 symbol 填 0（解码端按 statusCount 截断）
    for (size_t i = 0; i < count; i += 7) {
        uint8_t s[7] = {0, 0, 0, 0, 0, 0, 0};
        for (size_t j = 0; j < 7 && i + j < count; ++j) s[j] = symbols[i + j];
        body.push_back(static_cast<uint8_t>(0xC0 | (s[0] << 4) | (s[1] << 2) | s[2]));
        body.push_back(static_cast<uint8_t>((s[3] << 6) | (s[4] << 4) | (s[5] << 2) | s[6]));
    }

    // delta：到达时刻差分（250us 网格），首个相对 quantRef，其后链式相对上一个
    double prev = quantRefMs;
    for (const auto& kv : recv) {
        if (kv.first >= count) break;  // 超出 15 位窗口的极端样本丢弃
        double d = std::lround((kv.second - prev) * 4.0);
        if (d > 32767.0) d = 32767.0;
        if (d < -32768.0) d = -32768.0;
        putU16(body, static_cast<uint16_t>(static_cast<int16_t>(d)));
        prev = kv.second;
    }

    // ---- 5. 公共头 + 4 字节对齐 padding（RFC 3550：P 位 + 末字节计数）----
    // body 恒为偶数（16 固定 + 2 的倍数），padding 只会 0 或 2 字节
    size_t pad = (4 - body.size() % 4) % 4;

    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(0x80 | RTCP_FMT_TWCC | (pad ? 0x20 : 0)));
    out.push_back(RTCP_PT_RTPFB);
    putU16(out, static_cast<uint16_t>((4 + body.size() + pad) / 4 - 1));
    out.insert(out.end(), body.begin(), body.end());
    for (size_t i = 1; i < pad; ++i) out.push_back(0);  // padding 体补零
    if (pad) out.push_back(static_cast<uint8_t>(pad)); // 末字节 = padding 数

    return out;
}

// ============================================================================
// 解码：feedback 主体 → 结构化 TwccFeedback
// ============================================================================
bool parseTransportFeedback(const uint8_t* data, size_t len, TwccFeedback& fb) {
    // 固定主体：baseSeq(2) + statusCount|B(2) + refTime(3) + fbCount(1)
    if (len < 8) return false;

    fb.received.clear();
    fb.lost.clear();
    fb.baseSeq = getU16(data);

    // statusCount 15 位（byte4 全部 + byte5 高 7 位），B 位（byte5 最低位）恒 0 忽略
    size_t statusCount = static_cast<size_t>(getU16(data + 2)) >> 1;

    // refTime 24 位无符号 → 毫秒（1/64 网格）
    uint32_t refTime = (static_cast<uint32_t>(data[4]) << 16) |
                      (static_cast<uint32_t>(data[5]) << 8) | data[6];
    fb.refTimeMs = static_cast<double>(refTime) / 64.0;
    // data[7] = fb packet count，忽略

    // ---- symbol 流：逐 chunk 展开（每块 7 个 2-bit symbol）----
    std::vector<uint8_t> symbols;
    symbols.reserve(statusCount);
    size_t pos = 8;
    while (symbols.size() < statusCount && pos + 2 <= len) {
        uint8_t b0 = data[pos], b1 = data[pos + 1];
        // 只认 StatusVectorChunk：T=1（bit15）且 S=1（bit14，2-bit symbol）
        // RunLengthChunk（T=0）本实现不产出也不解析，遇到视为畸形
        if ((b0 >> 6) != 0b11) return false;
        pos += 2;
        const uint8_t chunkSyms[7] = {
            static_cast<uint8_t>((b0 >> 4) & 3),
            static_cast<uint8_t>((b0 >> 2) & 3),
            static_cast<uint8_t>(b0 & 3),
            static_cast<uint8_t>((b1 >> 6) & 3),
            static_cast<uint8_t>((b1 >> 4) & 3),
            static_cast<uint8_t>((b1 >> 2) & 3),
            static_cast<uint8_t>(b1 & 3)};
        for (int i = 0; i < 7 && symbols.size() < statusCount; ++i)
            symbols.push_back(chunkSyms[i]);
    }
    // chunk 数不匹配 statusCount 时以先到的为准（防御畸形，不失败）

    // ---- delta 链还原到达时刻 ----
    double arrival = fb.refTimeMs;
    for (size_t i = 0; i < symbols.size(); ++i) {
        uint16_t seq = static_cast<uint16_t>(fb.baseSeq + i);  // 回绕安全
        uint8_t sym = symbols[i];
        if (sym == kSymbolLargeDelta) {  // 2 字节有符号 delta（250us）
            if (pos + 2 > len) return false;  // delta 缺失 = 硬错位
            int16_t d = static_cast<int16_t>((data[pos] << 8) | data[pos + 1]);
            pos += 2;
            arrival += d * 0.25;
            fb.received.push_back({seq, arrival});
        } else if (sym == kSymbolSmallDelta) {  // 1 字节无符号（兼容 draft）
            if (pos + 1 > len) return false;
            arrival += data[pos] * 0.25;
            pos += 1;
            fb.received.push_back({seq, arrival});
        } else {  // 00 未收到 / 11 保留
            fb.lost.push_back(seq);
        }
    }
    return true;
}

} // namespace crystal
