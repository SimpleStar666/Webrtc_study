// ============================================================================
// nack_requester.h - 接收端 NACK 请求状态机
// ============================================================================
//
// 【职责】
// 接收 JitterBuffer 检测到的丢失序列号，管理"请求 → 重试 → 放弃"状态：
//   新间隙   → 立即请求（onMissing 返回值即本次要发的 seq）
//   未恢复   → 每 33ms（约一个视频帧周期）重试，上限 3 次
//   超上限   → 放弃（大概率真丢包，画面恢复交给 PLI 兜底）
//   包到达   → 移除待请求条目（重传成功或原本乱序）
//
// 【参数权衡（面试考点）】
//   重试上限 3：NACK 往返一次 ≈ RTT；30fps 下 33ms 间隔 × 3 次 ≈ 100ms，
//   超过此窗口接收端缓冲早已输出该时间片，继续重传无意义。
//
// 【驱动方式】
//   onMissing/onReceived 由接收路径（onTrack 回调）调用；
//   tick 由上层周期调用（main_client 主循环每 16ms 驱动一次）。
// ============================================================================

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace crystal {

class NackRequester {
public:
    static constexpr int kMaxRetries = 3;          // 含首次请求的总次数
    static constexpr uint64_t kRetryIntervalMs = 33;

    // 登记新检测到的丢失 seq；返回需要"立即"请求的 seq（首次请求）
    std::vector<uint16_t> onMissing(const std::vector<uint16_t>& gaps,
                                    uint64_t nowMs);
    // 周期驱动；返回到期需要重试的 seq
    std::vector<uint16_t> tick(uint64_t nowMs);
    // 任意 RTP 包到达（含重传/乱序）时调用，解除待请求
    void onReceived(uint16_t seq);

    // 统计：累计发出的请求次数（含重试）
    uint64_t requestedCount() const;
    // 统计：重试耗尽仍未恢复、最终放弃的 seq 数
    uint64_t givenUpCount() const;

private:
    struct Item {
        uint8_t tries = 1;        // 已请求次数（登记即第 1 次）
        uint64_t lastReqMs = 0;   // 上次请求时刻
    };

    mutable std::mutex mutex_;
    std::map<uint16_t, Item> pending_;  // key = 丢失序列号
    uint64_t requested_ = 0;
    uint64_t givenUp_ = 0;
};

} // namespace crystal
