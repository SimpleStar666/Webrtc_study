// ============================================================================
// gcc_controller.cpp - GCC 带宽估计控制器实现
// ============================================================================
//
// 【数据流】
//   onFeedback: (arrival, owd) 样本 → 累积 OWD 差分 → trendline 窗口
//   onLossUpdate: RR 丢包率 → 硬通道立即生效（降/持平/增长标志）
//   tick: trendline 斜率 → 过载状态机（3 轮去抖）→ AIMD 增长
//
// 【为什么 ×0.85 / +8%】
// 乘性减 15%：一次退避足够让队列排空又不过度浪费带宽（TCP Reno 是减半，
// GCC 温和得多——实时视频对码率骤降更敏感，会立刻掉画质）。
// 加性增 8%：指数式缓慢爬升探测剩余带宽，40kbps 下限保证低速链路也有
// 可感知的恢复速度。
// ============================================================================

#include "media/gcc/gcc_controller.h"
#include <algorithm>

namespace crystal {

namespace {
constexpr uint32_t kMinKbps = 100;      // 钳制下限（语音级可用码率）
constexpr uint32_t kMaxKbps = 4000;     // 钳制上限（720p 高码率天花板）
constexpr double kLossHigh = 0.10;      // 丢包 >10%：强降
constexpr double kLossLow = 0.02;       // 丢包 <2%：允许增长
constexpr double kOveruseSlope = 0.01;  // 斜率阈值：每 ms 时间涨 0.01ms 延迟
constexpr double kRecoverSlope = -0.01; // 斜率恢复线（延迟下降）
constexpr int kOveruseStreakLimit = 3;   // 连续过载次数（去抖）
} // namespace

GccController::GccController(uint32_t initKbps)
    : trendline_(20),          // 20 样本回归窗口（libwebrtc 默认量级）
      targetKbps_(std::clamp(initKbps, kMinKbps, kMaxKbps)) {}

// ============================================================================
// 丢包通道（硬规则，立即生效）
// ============================================================================
void GccController::onLossUpdate(double lossRate) {
    lastLoss_ = lossRate;
    if (lossRate > kLossHigh) {
        applyDecrease();        // >10%：强降（网络已在丢，硬通道让路）
    } else if (lossRate < kLossLow) {
        applyIncrease();        // <2%：健康，加性增
    }
    // 2%~10%：持平——轻度丢包可能是瞬时抖动，交给趋势通道判断
    clamp();
}

// ============================================================================
// 趋势通道（喂 trendline）
// ============================================================================
void GccController::onFeedback(const FeedbackSample& s) {
    size_t n = std::min(s.arrivals.size(), s.owdMs.size());
    for (size_t i = 0; i < n; ++i) {
        double owd = s.owdMs[i];
        // OWD 差分：消掉收发时钟偏移（绝对值无意义，增量才是排队变化）
        // 跨批衔接：lastOwd_ 保留上一批末尾样本，曲线不断裂
        if (hasLast_) accumDelay_ += owd - lastOwd_;
        lastOwd_ = owd;
        hasLast_ = true;
        trendline_.update(s.arrivals[i].arrivalMs, accumDelay_);
    }
}

// ============================================================================
// 周期 tick：过载状态机 + AIMD 增长
// ============================================================================
void GccController::tick() {
    bool reduced = false;

    if (trendline_.ready()) {
        double slope = trendline_.slope();
        if (slope > kOveruseSlope) {
            ++overuseStreak_;                    // 延迟在涨：过载嫌疑 +1
        } else if (slope < kRecoverSlope) {
            overuseStreak_ = 0;                   // 延迟在降：恢复，撤销嫌疑
        }
        // 平稳区间（-0.01 ~ 0.01）：嫌疑保持（既不累计也不清零）

        if (overuseStreak_ >= kOveruseStreakLimit) {
            applyDecrease();                      // 连续 3 轮过载：降
            overuseStreak_ = 0;                   // 重新计一轮去抖周期
            reduced = true;
        }
    }

    // 加性增：本 tick 没降过 && 丢包通道健康
    if (!reduced && lastLoss_ < kLossLow) {
        applyIncrease();
    }
    clamp();
}

// ============================================================================
// AIMD 操作
// ============================================================================
void GccController::applyDecrease() {
    // 整数算术：1000×85/100 = 850 精确；浮点 0.85 无精确二进制表示会得 849
    targetKbps_ = targetKbps_ * 85 / 100;
}

void GccController::applyIncrease() {
    uint32_t inc = std::max<uint32_t>(40, targetKbps_ * 8 / 100);
    targetKbps_ += inc;
}

void GccController::clamp() {
    targetKbps_ = std::clamp(targetKbps_, kMinKbps, kMaxKbps);
}

} // namespace crystal
