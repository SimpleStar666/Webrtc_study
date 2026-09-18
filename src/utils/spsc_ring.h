// ============================================================================
// spsc_ring.h - 单生产者单消费者无锁环形缓冲（工程化升级 v2 新增）
// ============================================================================
//
// 【为什么需要无锁】
//   SDL 音频回调运行在 SDL 内部的高优先级实时线程上。如果回调里抢
//   mutex，而锁恰好被解码线程持有（比如 play() 里正在扩容队列），
//   回调就可能被优先级反转/调度延迟卡住——音频设备等着填数据，等不
//   到就播出爆音。实时系统铁律：回调路径上不能有锁。
//
//   SPSC（Single-Producer Single-Consumer）是唯一能完全无锁的队列
//   形态：只有一个线程写、只有一个线程读，两个方向各只需一个原子
//   计数器，不存在多方竞争，也就不需要 CAS 重试。
//
// 【实现原理】
//   两个永远递增的 uint64 计数器，槽位下标 = 计数器 % capacity：
//     writeCount_：写方累计写入数（写方独占修改）
//     readCount_ ：读方累计读出数（读方独占修改）
//
//   水位 = writeCount_ - readCount_（写方/读方各自看到的一致快照）
//
//   永不回绕：uint64 每纳秒加 1 也要 584 年才溢出，模运算安全。
//   （对比经典的"指针 == 容量时归零"方案：满和空两种状态无法区分，
//   需要 sacrificial slot 或额外标志；计数器方案从根上避开。）
//
// 【内存序（本文件最核心的三行代码）】
//   写方：写槽位数据 → writeCount_.store(w, release)
//         release 保证：读方看到新 writeCount_ 时，槽位数据必然已就绪
//   读方：writeCount_.load(acquire) → 读槽位数据 → readCount_.store(r, release)
//         acquire 保证：读到新 writeCount_ 后读槽位，看到的是完整数据
//         readCount_ 的 release 是写给写方看的：覆盖槽位前先确认旧数据已读走
//   写方覆盖槽位前：readCount_.load(acquire) 与之配对
//
//   这就是 release-acquire 配对：两个原子变量像两道闸门，把普通内存
//   读写"夹"出跨线程可见性保证。少了它，x86 上碰巧能跑（强内存序），
//   ARM 上必然偶发撕裂数据。
//
// 【溢出策略——为什么丢最旧必须由消费者执行】
//   "缓冲满了丢最旧"直觉上该由写方做（读指针前移腾位置），但读指针
//   归读方独占：写方 fetch_add 前移它，可能与读方 pop 末尾的 store
//   竞争，把计数器拉回去 → 水位算出负数（size_t 下溢成天文数字）→
//   写方认为有海量空位 → 覆盖读方正读的数据 → 持续性撕裂。
//
//   因此本类的分工是：
//     · push() 有界写入：只写空闲槽位，放不下的"最新"部分丢弃并计数
//       （写方只写 writeCount_ 之后的槽位，与读方的读区间在绝对计数
//        空间天然不相交——无锁正确性的根基）
//     · drop() 消费者丢最旧：视频帧队列取最新帧、音频水位回落，
//       都在读方线程调用 drop()/pop() 完成——只动读方自己的指针，安全
//
// 【伪共享】
//   两个计数器若落在同一 64 字节缓存行，写方的 store 会让读方所在核
//   的缓存行反复失效（缓存一致性协议乒乓）。alignas(64) 把它们隔开。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace crystal {

template <typename T>
class SpscRing {
public:
    // capacity - 环形缓冲容量（元素个数），必须 > 0
    explicit SpscRing(size_t capacity)
        : slots_(capacity), capacity_(capacity) {}

    // ---- 生产者接口（仅一个线程调用）----

    // 批量写入至多 n 个元素（有界写入）
    // 返回实际写入数；放不下的最新部分被丢弃并计入 pushDropCount
    // 永不阻塞、永不覆盖未读数据
    size_t push(const T* items, size_t n) {
        uint64_t r = readCount_.load(std::memory_order_acquire);
        uint64_t w = writeCount_.load(std::memory_order_relaxed);
        size_t free = capacity_ - static_cast<size_t>(w - r);
        size_t actual = n < free ? n : free;
        for (size_t i = 0; i < actual; ++i) {
            slots_[(w + i) % capacity_] = items[i];
        }
        writeCount_.store(w + actual, std::memory_order_release);
        if (actual < n)
            pushDropped_ += n - actual;   // 仅写方访问，relaxed 即可
        return actual;
    }

    // 单元素写入（移动语义）——大对象（如视频帧的 vector）避免深拷贝
    // 缓冲满返回 false（该元素被丢弃并计入 pushDropCount）
    bool pushOne(T&& value) {
        uint64_t r = readCount_.load(std::memory_order_acquire);
        uint64_t w = writeCount_.load(std::memory_order_relaxed);
        size_t free = capacity_ - static_cast<size_t>(w - r);
        if (free == 0) {
            pushDropped_ += 1;
            return false;
        }
        slots_[w % capacity_] = std::move(value);
        writeCount_.store(w + 1, std::memory_order_release);
        return true;
    }

    // ---- 消费者接口（仅一个线程调用）----

    // 单元素读出（移动语义）——大对象（如视频帧 vector）避免拷贝
    // 缓冲空返回 false
    bool popOne(T& out) {
        uint64_t w = writeCount_.load(std::memory_order_acquire);
        uint64_t r = readCount_.load(std::memory_order_relaxed);
        if (w == r) return false;
        out = std::move(slots_[r % capacity_]);
        readCount_.store(r + 1, std::memory_order_release);
        return true;
    }

    // 批量读出至多 n 个元素（从最旧开始）
    // 返回实际读出数（可能 < n，缓冲不足由调用方决定如何填充）
    size_t pop(T* out, size_t n) {
        uint64_t w = writeCount_.load(std::memory_order_acquire);
        uint64_t r = readCount_.load(std::memory_order_relaxed);
        size_t avail = static_cast<size_t>(w - r);
        size_t actual = n < avail ? n : avail;
        for (size_t i = 0; i < actual; ++i) {
            out[i] = slots_[(r + i) % capacity_];
        }
        readCount_.store(r + actual, std::memory_order_release);
        return actual;
    }

    // 消费者丢弃至多 n 个最旧元素（丢最旧策略的安全实现位置）
    // 返回实际丢弃数。用于：音频水位回落（防延迟爬升）、视频帧
    // 队列取最新帧（丢弃积压的过期帧）
    size_t drop(size_t n) {
        uint64_t w = writeCount_.load(std::memory_order_acquire);
        uint64_t r = readCount_.load(std::memory_order_relaxed);
        size_t avail = static_cast<size_t>(w - r);
        size_t actual = n < avail ? n : avail;
        readCount_.store(r + actual, std::memory_order_release);
        if (actual > 0)
            consumerDropped_ += actual;   // 仅消费方访问
        return actual;
    }

    // ---- 观测接口（任意线程，近似值）----

    // 当前水位（元素个数）。写方/读方各自看到的快照可能略滞后，
    // 对水位监控/指标上报完全够用
    size_t size() const {
        uint64_t w = writeCount_.load(std::memory_order_acquire);
        uint64_t r = readCount_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const { return size() == 0; }

    // 生产侧丢弃累计：缓冲满时放不下而被丢弃的最新元素总数
    uint64_t pushDropCount() const {
        return pushDropped_.load(std::memory_order_relaxed);
    }

    // 消费侧丢弃累计：drop() 主动丢弃的最旧元素总数
    uint64_t consumerDropCount() const {
        return consumerDropped_.load(std::memory_order_relaxed);
    }

    size_t capacity() const { return capacity_; }

private:
    // 槽位存储。元素默认构造（T 为 int16_t/vector 等均开销可忽略）
    std::vector<T> slots_;
    const size_t capacity_;

    // 计数器分属不同缓存行，避免伪共享（见文件头注释）
    alignas(64) std::atomic<uint64_t> writeCount_{0};  // 写方独占修改
    alignas(64) std::atomic<uint64_t> readCount_{0};   // 读方独占修改

    // 丢弃计数：pushDropCount_ 仅写方线程访问，consumerDropped_ 仅
    // 消费方线程访问；观测线程 relaxed 读（纯统计量，不保护任何数据）
    alignas(64) std::atomic<uint64_t> pushDropped_{0};
    alignas(64) std::atomic<uint64_t> consumerDropped_{0};
};

} // namespace crystal
