// ============================================================================
// trendline_estimator.h - Trendline 延迟梯度估计器（v2 新增）
// ============================================================================
//【为什么用斜率不用原始 OWD】
// 收发两端时钟不同步，原始单向延迟（OWD）的绝对值无意义（含未知时钟偏移）。
// 但"相邻包 OWD 的差分"消掉了偏移：排队延迟增加 → OWD 在增长 → 拥塞。
// 对 (到达时刻, 累积 OWD 差分) 做最小二乘回归，斜率即延迟增长率。
// libwebrtc 同款思路（modules/remote_bitrate_estimator/trendline_estimator.cc）。
//
//【最小二乘公式】
//   slope = Σ(t-t̄)(y-ȳ) / Σ(t-t̄)²
//         = (n·Σty - Σt·Σy) / (n·Σt² - (Σt)²)
// 用滚动和（Σt、Σy、Σtt、Σty）维护窗口，每次 update O(1)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace crystal {

class TrendlineEstimator {
public:
    // window：回归窗口的样本数（libwebrtc 默认 20）
    explicit TrendlineEstimator(size_t window = 20);

    // 喂一个样本：arrivalMs 到达时刻，delayMs 累积延迟（差分后累加值）
    void update(double arrivalMs, double delayMs);

    // 回归斜率（delay 增量 / 到达时刻增量，>0 延迟在涨）
    // 样本不足窗口或分母为 0（所有样本同一时刻）时返回 0
    double slope() const;

    // 样本数达到窗口要求了吗（不足时 slope() 不可信）
    bool ready() const;

private:
    size_t window_;                       // 样本窗口
    std::deque<double> t_, y_;            // 样本
    double sumT_ = 0, sumY_ = 0;          // 滚动和（避免每次全量重算）
    double sumTT_ = 0, sumTY_ = 0;
};

} // namespace crystal
