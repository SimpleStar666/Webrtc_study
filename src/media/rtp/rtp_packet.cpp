// ============================================================================
// rtp_packet.cpp - RTP 数据包的解析与序列化实现
// ============================================================================
//
// 本文件实现了 RtpPacket 类的核心功能：从网络字节流解析 RTP 包以及将
// RtpPacket 对象序列化为网络字节流。
//
// 【核心概念】
// 1. 网络字节序（大端序）: RTP 协议规定所有多字节字段使用大端序传输，
//    即最高有效字节在前。因此在解析时需要从字节流中按大端序重组多字节值，
//    在序列化时需要按大端序拆分多字节值。
//
// 2. 位操作: RTP 头部的第一个字节包含多个位域（V/P/X/CC），
//    第二个字节包含 M 和 PT，需要通过位掩码和移位操作来提取/组装。
//
// 3. 头部长度计算: RTP 固定头部为 12 字节，如果存在 CSRC 标识符，
//    则头部长度 = 12 + CC * 4 字节。如果存在扩展头部，还需加上扩展部分的长度。
// ============================================================================

#include "media/rtp/rtp_packet.h"
#include "utils/logger.h"
#include <cstring>
#include <stdexcept>

namespace crystal {

// 默认构造函数，所有成员使用声明时的默认初始值
RtpPacket::RtpPacket() = default;

// ============================================================================
// parse - 从原始字节流解析 RTP 数据包
// ============================================================================
//
// 【算法思路】
// RTP 包在网络中以大端序（Big-Endian）传输，解析过程就是按照 RFC 3550
// 定义的头部格式，逐字节提取各字段值。
//
// 【步骤详解】
// 1. 长度校验：RTP 固定头部至少 12 字节
// 2. 版本校验：RTP 版本号必须为 2（RFC 3550 规定）
// 3. 计算实际头部长度：固定头部 12 字节 + CSRC 部分 (CC * 4) 字节
// 4. 提取头部各字段（大端序 → 主机序）
// 5. 复制负载数据
//
// 【关于大端序转换】
// 网络字节序为大端序（高位字节在前），而 x86/ARM 等 CPU 通常使用小端序。
// 因此需要手动将字节流重组为多字节整数：
//   uint16_t value = (byte0 << 8) | byte1;
//   uint32_t value = (byte0 << 24) | (byte1 << 16) | (byte2 << 8) | byte3;
bool RtpPacket::parse(const uint8_t* data, size_t size) {
    // 第一步：检查最小长度
    // RTP 固定头部为 12 字节（V/P/X/CC 1字节 + M/PT 1字节 + Seq 2字节 + TS 4字节 + SSRC 4字节）
    if (size < 12) {
        Logger::error("RTP packet too short: {} bytes", size);
        return false;
    }

    // 第二步：提取头部前两个字节
    // byte0 包含: V(2bit) | P(1bit) | X(1bit) | CC(4bit)
    // byte1 包含: M(1bit) | PT(7bit)
    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];

    // 第三步：验证版本号
    // byte0 >> 6 提取最高 2 位，即版本号 V
    // RFC 3550 规定 V 必须为 2，旧版 RTP（V=0）和 VAT（V=1）已弃用
    if ((byte0 >> 6) != 2) {
        Logger::error("Invalid RTP version: {}", byte0 >> 6);
        return false;
    }

    // 第四步：提取 CSRC 计数并计算头部长度
    // byte0 & 0x0F 提取最低 4 位，即 CC 字段
    // 每个 CSRC 标识符占 4 字节，所以 CSRC 部分总长 = CC * 4
    uint8_t cc = byte0 & 0x0F;
    size_t header_len = 12 + cc * 4;

    // 检查数据长度是否足够容纳完整头部（包括 CSRC）
    if (size < header_len) {
        Logger::error("RTP packet too short for CSRC: {} < {}", size, header_len);
        return false;
    }

    // 第五步：提取标记位和负载类型
    // byte1 & 0x80: 提取最高位 M（标记位），0x80 = 1000 0000
    // byte1 & 0x7F: 提取低 7 位 PT（负载类型），0x7F = 0111 1111
    marker_ = (byte1 & 0x80) != 0;
    payloadType_ = byte1 & 0x7F;

    // 第六步：提取序列号（16位，大端序）
    // data[2] 是序列号的高字节，data[3] 是低字节
    // static_cast<uint16_t> 确保移位操作在 16 位范围内进行
    sequenceNumber_ = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    // 第七步：提取时间戳（32位，大端序）
    // data[4]~data[7] 依次为时间戳的最高有效字节到最低有效字节
    // static_cast<uint32_t> 确保移位操作在 32 位范围内进行
    timestamp_ = (static_cast<uint32_t>(data[4]) << 24) |
                 (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) |
                 data[7];

    // 第八步：提取 SSRC（32位，大端序）
    // data[8]~data[11] 依次为 SSRC 的最高有效字节到最低有效字节
    ssrc_ = (static_cast<uint32_t>(data[8]) << 24) |
            (static_cast<uint32_t>(data[9]) << 16) |
            (static_cast<uint32_t>(data[10]) << 8) |
            data[11];

    // 第九步：提取负载数据
    // 负载从头部之后开始，长度 = 总长度 - 头部长度
    size_t payload_len = size - header_len;
    if (payload_len > 0) {
        payload_.assign(data + header_len, data + header_len + payload_len);
    } else {
        payload_.clear();
    }

    return true;
}

// ============================================================================
// serialize - 将 RTP 数据包序列化为原始字节流
// ============================================================================
//
// 【算法思路】
// 将 RtpPacket 对象的各成员变量按照 RFC 3550 定义的格式组装为字节流。
// 这是 parse 的逆操作：将主机序的多字节整数拆分为大端序的字节序列。
//
// 【步骤详解】
// 1. 组装 Byte0: 版本号(2) 左移6位 | P(0) | X(0) | CC(0)
// 2. 组装 Byte1: M 左移7位 | PT
// 3. 按大端序写入序列号（2字节）
// 4. 按大端序写入时间戳（4字节）
// 5. 按大端序写入 SSRC（4字节）
// 6. 追加负载数据
std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(12 + payload_.size());

    // 组装 Byte0: V=2(占bit7-6) | P=0(bit5) | X=0(bit4) | CC=0(bit3-0)
    // 2 << 6 = 0x80 = 1000 0000，即版本号 2 占据最高 2 位
    uint8_t byte0 = (2 << 6) | 0x00;
    buf.push_back(byte0);

    // 组装 Byte1: M(占bit7) | PT(占bit6-0)
    // marker_ ? 0x80 : 0x00 控制标记位，0x80 = 1000 0000
    // payloadType_ & 0x7F 确保只取低 7 位
    uint8_t byte1 = (marker_ ? 0x80 : 0x00) | (payloadType_ & 0x7F);
    buf.push_back(byte1);

    // 序列号：按大端序拆分为 2 字节
    // 高字节 = seq >> 8，低字节 = seq & 0xFF
    buf.push_back(static_cast<uint8_t>(sequenceNumber_ >> 8));
    buf.push_back(static_cast<uint8_t>(sequenceNumber_ & 0xFF));

    // 时间戳：按大端序拆分为 4 字节
    // 从最高有效字节到最低有效字节依次写入
    buf.push_back(static_cast<uint8_t>(timestamp_ >> 24));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(timestamp_ & 0xFF));

    // SSRC：按大端序拆分为 4 字节
    buf.push_back(static_cast<uint8_t>(ssrc_ >> 24));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ssrc_ & 0xFF));

    // 追加负载数据
    buf.insert(buf.end(), payload_.begin(), payload_.end());
    return buf;
}

// ============================================================================
// Getter 方法实现
// ============================================================================

// 版本号固定返回 2（RFC 3550 规定的唯一有效版本）
uint8_t RtpPacket::version() const { return 2; }

// 填充标志：当前实现不支持填充，固定返回 false
bool RtpPacket::padding() const { return false; }

// 扩展标志：当前实现不支持头部扩展，固定返回 false
bool RtpPacket::extension() const { return false; }

// CSRC 计数：当前实现不支持 CSRC，固定返回 0
uint8_t RtpPacket::csrcCount() const { return 0; }

// 标记位：返回解析或设置的标记位值
bool RtpPacket::marker() const { return marker_; }

// 负载类型：返回 7 位负载类型值
uint8_t RtpPacket::payloadType() const { return payloadType_; }

// 序列号：返回 16 位序列号
uint16_t RtpPacket::sequenceNumber() const { return sequenceNumber_; }

// 时间戳：返回 32 位时间戳
uint32_t RtpPacket::timestamp() const { return timestamp_; }

// SSRC：返回 32 位同步源标识符
uint32_t RtpPacket::ssrc() const { return ssrc_; }

// 负载数据：返回负载的只读引用
const std::vector<uint8_t>& RtpPacket::payload() const { return payload_; }

// ============================================================================
// Setter 方法实现
// ============================================================================

void RtpPacket::setMarker(bool m) { marker_ = m; }

// 设置负载类型，& 0x7F 确保只取低 7 位，防止误设置标记位
void RtpPacket::setPayloadType(uint8_t pt) { payloadType_ = pt & 0x7F; }

void RtpPacket::setSequenceNumber(uint16_t seq) { sequenceNumber_ = seq; }
void RtpPacket::setTimestamp(uint32_t ts) { timestamp_ = ts; }
void RtpPacket::setSsrc(uint32_t ssrc) { ssrc_ = ssrc; }

// 设置负载数据（vector 版本）：直接拷贝
void RtpPacket::setPayload(const std::vector<uint8_t>& data) {
    payload_ = data;
}

// 设置负载数据（指针+长度版本）：从原始内存拷贝
void RtpPacket::setPayload(const uint8_t* data, size_t len) {
    payload_.assign(data, data + len);
}

// ============================================================================
// 大小计算
// ============================================================================

// 头部大小固定为 12 字节（不含 CSRC 和扩展头部）
size_t RtpPacket::headerSize() const { return 12; }

// 总大小 = 固定头部 12 字节 + 负载长度
size_t RtpPacket::totalSize() const { return 12 + payload_.size(); }

} // namespace crystal
