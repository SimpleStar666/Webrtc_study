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
