#include "media/adaptive/adaptation_controller.h"
#include <algorithm>

namespace crystal {

AdaptationController::AdaptationController(uint32_t configuredKbps,
                                           uint32_t configuredFps)
    : configuredKbps_(configuredKbps),
      configuredFps_(configuredFps),
      ladder_(buildLadder(configuredFps)),
      targetFps_(configuredFps) {
    decision_.targetFps = configuredFps;
    decision_.level = NetworkQuality::Good;
}

std::vector<uint32_t> AdaptationController::buildLadder(uint32_t fps) {
    // 阶梯比例 {100%, 2/3, 50%, 40%, 33%}，逐级钳到下限并去重
    // 30fps → {30, 20, 15, 12, 10}（设计文档同款）
    const double kFractions[] = {1.0, 2.0 / 3.0, 0.5, 0.4, 1.0 / 3.0};
    std::vector<uint32_t> ladder;
    for (double f : kFractions) {
        uint32_t v = std::max(kMinFps,
                              static_cast<uint32_t>(fps * f + 0.5));
        if (ladder.empty() || v < ladder.back())
            ladder.push_back(v);   // 严格降序（低帧率配置会自然缩短阶梯）
    }
    return ladder;
}

uint32_t AdaptationController::fecCapFor(NetworkQuality q) {
    switch (q) {
        case NetworkQuality::Poor: return 50;  // 丢包真实升高：放宽
        case NetworkQuality::Bad:  return 30;  // 链路受限：防 FEC 挤占载荷
        default:                   return 40;  // Good/Fair
    }
}

size_t AdaptationController::currentRung() const {
    for (size_t i = 0; i < ladder_.size(); ++i)
        if (ladder_[i] == targetFps_) return i;
    return 0;  // 不在阶梯上：按最高档处理（下一轮升降会归位）
}

void AdaptationController::applyLadder(NetworkQuality level, uint64_t nowMs) {
    size_t idx = currentRung();

    if (level == NetworkQuality::Poor || level == NetworkQuality::Bad) {
        // 降级跳档：Poor → 减半档；Bad → 下限（实时视频宁可降质不可断流）
        uint32_t want = (level == NetworkQuality::Bad)
                            ? ladder_.back()
                            : std::max(kMinFps, configuredFps_ / 2);
        for (size_t i = 0; i < ladder_.size(); ++i) {
            if (ladder_[i] <= want && ladder_[i] < targetFps_) {
                targetFps_ = ladder_[i];
                lastLadderMoveMs_ = nowMs;
                return;
            }
        }
        return;  // 已在目标档或更低：不动（防重复移档刷新 dwell 计时）
    }

    // 升级：Good/Fair 逐级爬，每级至少停留 kMinDwellMs
    if (idx > 0 && nowMs - lastLadderMoveMs_ >= kMinDwellMs) {
        targetFps_ = ladder_[idx - 1];
        lastLadderMoveMs_ = nowMs;
    }
}

AdaptationDecision AdaptationController::tick(const NetworkSignals& s,
                                              uint64_t nowMs) {
    NetworkQuality raw = classifyNetwork(s);

    // --- 去抖状态机：快降慢升 ---
    if (raw != level_) {
        if (raw == candidate_) {
            ++candidateStreak_;
        } else {
            candidate_ = raw;
            candidateStreak_ = 1;
        }
        // 枚举数值序 = 劣化序（Good=0 … Bad=3）
        bool worse = static_cast<int>(raw) > static_cast<int>(level_);
        int needed = worse ? kDowngradeTicks : kUpgradeTicks;
        if (candidateStreak_ >= needed) {
            level_ = candidate_;
            candidateStreak_ = 0;
            levelSinceMs_ = nowMs;
            if (level_ == NetworkQuality::Bad)
                pliPending_ = true;   // 进入 Bad：请求关键帧恢复画面
        }
    } else {
        candidate_ = level_;
        candidateStreak_ = 0;         // 回到当前级：计数清零（防乒乓核心）
    }

    // --- 帧率阶梯 ---
    applyLadder(level_, nowMs);

    // --- 组装决策 ---
    decision_.level = level_;
    decision_.targetFps = targetFps_;
    decision_.fecCapPct = fecCapFor(level_);
    decision_.levelAgeMs = nowMs - levelSinceMs_;
    decision_.requestPli = pliPending_;
    pliPending_ = false;              // edge：暴露一次即消费
    return decision_;
}

} // namespace crystal
