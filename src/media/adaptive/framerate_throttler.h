// ============================================================================
// framerate_throttler.h - 帧率节流器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
//【在自适应链路中的角色】
// AdaptationController 决策 targetFps 后，由本执行器在编码线程落地：
// 距上一帧不足 1000/targetFps 的采集帧按策略丢弃。
//
//【与 Phase D backlog 丢帧的正交性（面试考点）】
//   backlog 丢帧：SpscRing 积压满（编码跟不上采集）——被动、编码器问题
//   policy 丢帧 ：带宽不够主动降帧率（本类）——主动、网络策略
//   两者都丢帧但含义完全不同，[net] 行分别计数。
//
//【为什么不重配编码器（degradation preference）】
// libwebrtc 降帧率走编码前丢帧（VideoStreamEncoder 的 frame dropper），
// 不动编码器上下文：重配 x264（分辨率/帧率参数）需要重建上下文，
// 重配瞬间可能卡顿。分辨率降级才走 ReconfigureEncoder 重路径。
//
//【线程模型】
// setTargetFps 主循环 store / shouldEncode 编码线程 load（原子交接）；
// lastEncodedMs_/policyDropped_ 仅编码线程访问，无需同步。
//
//【整数除法说明】
// 1000/fps 向下取整（30→33ms），30fps 源按 33ms 帧距刚好全部放行，
// 不会误丢；10fps 时源帧距 33ms 的 3 倍是 99<100，实际 ~7.6fps
// 轻微欠调——教学实现可接受，精确版可用微秒时钟。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

namespace crystal {

class FramerateThrottler {
public:
    explicit FramerateThrottler(uint32_t initialFps)
        : targetFps_(initialFps) {}

    // 主循环：Adaptation 决策发布新目标帧率
    void setTargetFps(uint32_t fps) {
        targetFps_.store(fps, std::memory_order_relaxed);
    }
    uint32_t targetFps() const {
        return targetFps_.load(std::memory_order_relaxed);
    }

    // 编码线程：每帧编码前调用；false = 本帧按策略丢弃
    bool shouldEncode(uint64_t nowMs) {
        uint32_t fps = targetFps_.load(std::memory_order_relaxed);
        if (fps == 0) return true;               // 防御：0 = 不节流
        uint64_t minIntervalMs = 1000 / fps;
        if (hasLast_ && nowMs - lastEncodedMs_ < minIntervalMs) {
            ++policyDropped_;
            return false;
        }
        lastEncodedMs_ = nowMs;
        hasLast_ = true;
        return true;
    }

    // 观测：策略丢帧累计（区别于 frameQueue 的 backlog 丢帧）
    uint64_t policyDroppedCount() const { return policyDropped_; }

private:
    std::atomic<uint32_t> targetFps_;
    bool hasLast_ = false;          // 仅编码线程访问
    uint64_t lastEncodedMs_ = 0;     // 仅编码线程访问
    uint64_t policyDropped_ = 0;    // 仅编码线程访问
};

} // namespace crystal
