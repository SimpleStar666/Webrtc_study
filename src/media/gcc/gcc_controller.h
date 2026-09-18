// ============================================================================
// gcc_controller.h - GCC 带宽估计控制器（v2 新增）
// ============================================================================
// 双通道（libwebrtc 同款思路）：
//   丢包通道：loss > 10% → 强降（×0.85）；2%~10% → 持平；< 2% → 可增长
//             （丢包是拥塞的"已发生"证据，硬规则，优先级高）
//   趋势通道：TWCC feedback → OWD 差分累积 → trendline 斜率 → 过载检测
//             （延迟增长先于丢包发生，是拥塞的"将发生"预警，精细化控制）
// AIMD 速率控制：
//   过载：bitrate × 0.85（乘性减，快速让路）
//   正常增长：+max(40, 8%×bitrate) kbps（加性增，试探带宽）
//   钳制 [100, 4000] kbps
// 过载去抖：连续 3 次 tick 斜率超阈值才降（避免单次抖动误杀）。
//
//【线程模型（工程化升级 v2：三级解耦后）】
// onFeedback/onLossUpdate 在 RTCP 接收线程调用，tick/查询在主循环调用
// ——内部一把 mutex 保护全部状态。事件率低（每秒几十次），锁开销可忽略；
// 这不是热路径，无锁化收益远小于复杂度代价（见指南 Module 12）。
//
// 生产差异（指南展开）：自适应过载阈值、ProbeController 探测、PacedSender。
#pragma once

#include "media/gcc/trendline_estimator.h"
#include "media/rtcp/transport_feedback.h"  // ArrivalSample
#include <cstdint>
#include <mutex>
#include <vector>

namespace crystal {

// 一次 feedback 的还原样本（main_client 从 RTCP 解出后组装）：
// arrival 来自 feedback 解码，owd = arrival - 本地记录的发送时刻
// （收发两机时钟不同步没关系——趋势只用差分，偏移被消掉）
struct FeedbackSample {
    std::vector<ArrivalSample> arrivals;  // (seq, arrivalMs)，按 seq 升序
    std::vector<double> owdMs;            // 与 arrivals[i] 对齐的单向延迟（ms）
};

class GccController {
public:
    explicit GccController(uint32_t initKbps);

    // 丢包通道输入（RR 报告块 fraction lost / 255）
    void onLossUpdate(double lossRate);

    // 趋势通道输入：一批 feedback 样本（内部喂 trendline 累积 OWD 差分）
    void onFeedback(const FeedbackSample& s);

    // 周期 tick（如每 100ms）：应用过载状态机 + AIMD 增长，产出新目标码率
    void tick();

    uint32_t targetBitrateKbps() const;

    // 当前趋势通道斜率（样本不足时 0；>0.01 延迟在涨，<-0.01 在恢复）
    // 观测用：[stats] 行 / Demo 打印，不参与控制
    double trendSlope() const;

private:
    // 以下三个操作均要求调用方已持锁（tick/onLossUpdate 内部路径）
    void applyDecrease();  // AIMD 乘性减：×0.85（整数算术避免浮点截断）
    void applyIncrease();  // AIMD 加性增：+max(40, 8%)
    void clamp();          // 钳制 [100, 4000]

    mutable std::mutex mutex_;  // RTCP 线程喂样本 vs 主循环 tick 并发
    TrendlineEstimator trendline_;
    uint32_t targetKbps_;
    int overuseStreak_ = 0;   // 连续过载计数（≥3 才降，去抖）
    double lastLoss_ = 0;     // 最近一次丢包率（0.02 以下才允许增长）
    double accumDelay_ = 0;   // 累积 OWD 差分（trendline 的 y 轴）
    double lastOwd_ = 0;      // 上一样本 OWD（跨批差分衔接）
    bool hasLast_ = false;    // 是否已有历史样本
};

} // namespace crystal
