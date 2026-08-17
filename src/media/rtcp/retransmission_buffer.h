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
