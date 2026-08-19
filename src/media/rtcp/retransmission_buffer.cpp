// ============================================================================
// retransmission_buffer.cpp - 重传缓冲区实现
// ============================================================================
//
// 【核心数据结构选择：std::deque（双端队列）】
//   - push_back 存新包          O(1)  ← 发送路径高频调用
//   - pop_front 淘汰最老包      O(1)  ← 淘汰只发生在队首
//   - 中间线性扫描按 seq 查找   O(n)  ← 只有收到 NACK 时才发生
//   deque 两端操作都是 O(1)，正好匹配"先进先出 + 偶尔查找"的使用模式。
//   如果换成 vector，队首淘汰要整体搬移数据；换成 map 查找更快但
//   淘汰逻辑更复杂——n 最多 512，线性扫描完全够用（真实工程会加 hash 索引）。
//
// 【为什么按插入序 ≈ 序列号递增序】
//   RTP 序列号由同一个发送循环单调递增分配，先发的包必然先 store，
//   所以队首永远是最老的包（回绕也不影响——淘汰只看时间/容量）。
// ============================================================================

#include "media/rtcp/retransmission_buffer.h"

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
// 两阶段初始化不需要：这里没有系统资源，直接保存参数即可。
// 默认值（512 包 / 3 秒）的由来见头文件注释。
RetransmissionBuffer::RetransmissionBuffer(size_t capacity, uint64_t ttlMs)
    : capacity_(capacity), ttlMs_(ttlMs) {}

// ============================================================================
// evictLocked - 淘汰过期/超容量的条目（内部方法，调用方必须已持锁）
// ============================================================================
//
// 【双重淘汰的执行顺序】先淘汰过期的，再淘汰超容量的：
//   1. TTL 淘汰：存了超过 3 秒的包直接丢弃——即使对端此刻来 NACK，
//      补发也已无意义（接收端早已放弃等待、画面已跳过该时间片）
//   2. 容量淘汰：TTL 淘汰后仍超过 512 个，说明短时间发包太猛
//      （如关键帧风暴），继续丢最老的保住内存上限
//
// 【为什么叫 xxxLocked 后缀】项目约定：带 Locked 的方法假设调用方
// 已持有 mutex_，自身不再加锁（避免重复加锁导致死锁）。
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

// ============================================================================
// store - 发送路径：每发出一个 RTP 包前，先存一份原始字节
// ============================================================================
//
// 【为什么存"序列化后的完整字节"而不是 RtpPacket 结构】
//   NACK 补发要求与原包逐字节一致（同 seq、同时间戳、同载荷）。
//   直接存字节 → 补发就是原样重发，零转换成本；
//   存结构体 → 补发前还得重新 serialize，多一次拷贝和出错机会。
//
// 【参数 packet 按值传递再 move】
//   调用方传右值时零拷贝移入，传左值时恰好一份拷贝进缓冲区，
//   这是 C++11 处理"要保存参数"的标准写法。
void RetransmissionBuffer::store(uint16_t seq, std::vector<uint8_t> packet,
                                 uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 顺手做一次淘汰，防止长期无 NACK 时缓冲区只增不减
    evictLocked(nowMs);
    entries_.push_back({seq, nowMs, std::move(packet)});
}

// ============================================================================
// get - RTCP 接收路径：对端 NACK 点名某个 seq，取出原包补发
// ============================================================================
//
// 【返回语义】
//   非空 vector → 命中，内容即原始 RTP 字节，调用方直接 sendMedia 补发
//   空 vector   → 未命中（包已被 TTL/容量淘汰，或音频包从未存过），
//                 计入 miss_ 统计——miss 增长说明 NACK 来得太迟或
//                 发送码率超出缓冲容量，是调参的信号
//
// 【为什么返回拷贝而不是引用】跨锁边界返回引用会悬空（锁释放后
//   其他线程可能淘汰该条目），拷贝一份最安全。
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

// ============================================================================
// 统计接口（[stats] 日志行的"重传 N miss M"两个字段的数据来源）
// ============================================================================
uint64_t RetransmissionBuffer::retransmittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retransmitted_;
}

uint64_t RetransmissionBuffer::missCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return miss_;
}

} // namespace crystal
