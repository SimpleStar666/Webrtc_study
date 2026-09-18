// ============================================================================
// retransmission_buffer.cpp - 重传缓冲区实现
// ============================================================================

#include "media/rtcp/retransmission_buffer.h"

namespace crystal {

RetransmissionBuffer::RetransmissionBuffer(size_t capacity, uint64_t ttlMs)
    : capacity_(capacity), ttlMs_(ttlMs) {}

// 淘汰（调用方持锁）：
//   1. TTL 淘汰：order_ 队首最老，逐个检查其存储时刻
//   2. 容量淘汰：仍超容量则继续淘汰最老
// 注意 order_ 里的 seq 可能已在 map 中被覆盖删除（重复 store），
// erase 时判 miss 是正常的——map.erase 语义对不存在的键是 no-op
void RetransmissionBuffer::evictLocked(uint64_t nowMs) {
    while (!order_.empty()) {
        uint16_t seq = order_.front();
        auto it = entries_.find(seq);
        if (it == entries_.end()) {
            order_.pop_front();          // 已被覆盖删除，仅清队列
            continue;
        }
        if (nowMs - it->second.tsMs > ttlMs_) {
            entries_.erase(it);
            order_.pop_front();
            continue;
        }
        break;                            // 队首未过期，后面的更新，停
    }
    while (entries_.size() > capacity_) {
        uint16_t seq = order_.front();
        order_.pop_front();
        entries_.erase(seq);
    }
}

void RetransmissionBuffer::store(uint16_t seq, std::vector<uint8_t> packet,
                                 uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    // 序列号正常递增不会重复；若重复（重置/回绕复用），覆盖旧数据
    if (entries_.find(seq) == entries_.end()) {
        order_.push_back(seq);
    }
    entries_[seq] = {nowMs, std::move(packet)};
}

// O(1) 命中（工程化升级 v2：原为 deque 线性扫描）
std::vector<uint8_t> RetransmissionBuffer::get(uint16_t seq, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    auto it = entries_.find(seq);
    if (it != entries_.end()) {
        retransmitted_++;
        return it->second.data;  // 拷贝原始字节
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
