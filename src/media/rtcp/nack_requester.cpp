// ============================================================================
// nack_requester.cpp - NACK 请求状态机实现
// ============================================================================
//
// 【一个丢失包的完整生命周期（时间线示例）】
//
//   t=0ms    JitterBuffer 发现 seq=101 丢失 → onMissing({101})
//            → 登记 {tries=1, lastReqMs=0}，立即发第 1 次 NACK
//
//   t=20ms   重传包到达（网络往返只花了 20ms）
//            → onReceived(101) → 从待请求表删除，皆大欢喜
//
//   ── 如果重传包一直没到 ──
//   t=33ms   tick() 发现距上次请求已过 33ms → 发第 2 次 NACK（tries=2）
//   t=66ms   tick() 再次到期 → 发第 3 次 NACK（tries=3，已达上限）
//   t=99ms   tick() 发现 tries >= 3 → 放弃，从表中删除，givenUp_++
//            → 上层看到放弃计数增长，发 PLI 请求关键帧兜底
//
// 【为什么重传包到了要调 onReceived 而不是等它自然过期】
//   提前删除能省下 2 次无谓的重试 NACK（每个 NACK 都占带宽），
//   这就是"任意 RTP 包到达都调 onReceived"的原因——宁可多调，不可漏调。
// ============================================================================

#include "media/rtcp/nack_requester.h"

namespace crystal {

// ============================================================================
// onMissing - 登记新检测到的丢失 seq，返回需要"立即"请求的部分
// ============================================================================
//
// 【调用时机】JitterBuffer::insert() 检测到新间隙后立刻调用。
//
// 【为什么返回值可能比入参少】
//   gaps 里可能混着"上次已登记、正在重试中"的 seq（比如乱序包来回触发
//   同一间隙的检测）。对已在表中的 seq 直接跳过，避免同一包重复登记、
//   重复请求——这也是状态机自带的"去重"能力，天然抑制 NACK 风暴。
//
// 【示例】
//   表中已有 {101(重试中)}，本次 gaps={101,105}
//   → 101 跳过，105 登记并立即请求 → 返回 {105}
std::vector<uint16_t> NackRequester::onMissing(const std::vector<uint16_t>& gaps,
                                               uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (uint16_t seq : gaps) {
        auto it = pending_.find(seq);
        if (it != pending_.end()) continue;  // 已在表中，交给 tick 重试
        // 登记即视为发出第 1 次请求（tries 从 1 起算，不是 0）
        pending_[seq] = Item{1, nowMs};
        requested_++;
        toRequest.push_back(seq);
    }
    return toRequest;
}

// ============================================================================
// tick - 周期驱动重试与放弃（由 main_client 主循环每 16ms 调一次）
// ============================================================================
//
// 对表中每个待请求条目做三选一判断：
//
//   距上次请求 < 33ms  → 未到期，跳过（防止同一个包被高频请求）
//   到期且 tries < 3   → 重试：tries++，返回给上层再发一次 NACK
//   到期且 tries >= 3  → 放弃：删除条目，givenUp_++（上层据此触发 PLI）
//
// 【为什么用 map 而不是 vector】
//   onMissing/onReceived 都要按 seq 快速查找/删除，map（红黑树）的
//   O(log n) 查找正合适；待请求条目通常只有个位数，性能绰绰有余。
//
// 【迭代中删除的技巧】
//   erase() 返回下一个迭代器，避免迭代器失效（C++ 标准写法）。
std::vector<uint16_t> NackRequester::tick(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (auto it = pending_.begin(); it != pending_.end();) {
        // 判断 1：还没到重试时间（33ms 间隔 ≈ 一个视频帧周期）
        if (nowMs - it->second.lastReqMs < kRetryIntervalMs) {
            ++it;  // 未到重试时间
            continue;
        }
        // 判断 2：重试次数耗尽 → 放弃
        if (it->second.tries >= kMaxRetries) {
            // 重试耗尽仍未恢复 → 放弃（PLI 兜底恢复画面）
            // 【为什么敢放弃】3 次请求 × 33ms 间隔 ≈ 100ms 还没回来，
            // 说明这个包大概率真丢了；即使现在补发成功，接收端播放
            // 时机也早已过去。让 PLI 请求关键帧重新开始更划算。
            givenUp_++;
            it = pending_.erase(it);
            continue;
        }
        // 判断 3：到期且还有重试机会 → 再请求一次
        it->second.tries++;
        it->second.lastReqMs = nowMs;
        requested_++;
        toRequest.push_back(it->first);
        ++it;
    }
    return toRequest;
}

// ============================================================================
// onReceived - 任意 RTP 包到达时调用，解除该 seq 的待请求状态
// ============================================================================
//
// 【为什么"任意"包都要调】到达的包可能是：
//   1. 请求的重传包     → 请求成功，删除条目 ✓
//   2. 原本乱序迟到的包 → 之前误判丢失，删除条目省掉重试 ✓
//   3. 与丢失无关的包   → 表中查不到，erase 无副作用 ✓
// 三种情况调它都无害，所以接收路径上"无条件调用"是最简单正确的写法。
void NackRequester::onReceived(uint16_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(seq);
}

// ============================================================================
// 统计接口（供 [stats] 日志行与测试使用，均加锁保证线程安全）
// ============================================================================
uint64_t NackRequester::requestedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_;
}

uint64_t NackRequester::givenUpCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return givenUp_;
}

} // namespace crystal
