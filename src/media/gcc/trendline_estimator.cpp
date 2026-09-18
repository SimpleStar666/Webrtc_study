// ============================================================================
// trendline_estimator.cpp - Trendline 延迟梯度估计器实现
// ============================================================================
//
// 【滚动窗口最小二乘】
// 维护 4 个滚动和 Σt、Σy、Σt²、Σty：新样本累加，超窗样本从队头扣除。
// 斜率公式用等价形式（免去二遍均值）：
//   slope = (n·Σty - Σt·Σy) / (n·Σt² - (Σt)²)
// 分母为 0（窗口内到达时刻全相同，样本退化）时返回 0。
// ============================================================================

#include "media/gcc/trendline_estimator.h"

namespace crystal {

TrendlineEstimator::TrendlineEstimator(size_t window)
    : window_(window < 2 ? 2 : window) {}  // 两点才能成线，下限 2

void TrendlineEstimator::update(double arrivalMs, double delayMs) {
    // 超窗滚出：从队头扣除该样本对 4 个滚动和的贡献
    if (t_.size() >= window_) {
        double ot = t_.front(), oy = y_.front();
        t_.pop_front();
        y_.pop_front();
        sumT_ -= ot;
        sumY_ -= oy;
        sumTT_ -= ot * ot;
        sumTY_ -= ot * oy;
    }

    t_.push_back(arrivalMs);
    y_.push_back(delayMs);
    sumT_ += arrivalMs;
    sumY_ += delayMs;
    sumTT_ += arrivalMs * arrivalMs;
    sumTY_ += arrivalMs * delayMs;
}

double TrendlineEstimator::slope() const {
    if (t_.size() < window_) return 0;  // 样本不足，不产出

    double n = static_cast<double>(t_.size());
    double denom = n * sumTT_ - sumT_ * sumT_;
    if (denom <= 0) return 0;           // 时刻全相同（退化），无斜率
    return (n * sumTY_ - sumT_ * sumY_) / denom;
}

bool TrendlineEstimator::ready() const { return t_.size() >= window_; }

} // namespace crystal
