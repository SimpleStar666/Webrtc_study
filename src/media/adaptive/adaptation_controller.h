// ============================================================================
// adaptation_controller.h - 网络自适应控制器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
// 【职责与分层（对齐 libwebrtc Call 级）】
//   GCC       ：带宽估计（targetKbps 是多少）—— 不动，本层只消费
//   本控制器  ：跨流策略（带宽不够时帧率降多少、FEC 上限收到哪、
//               要不要 PLI）—— 网络状态 → 统一决策
//   执行器    ：FramerateThrottler（编码线程）/ opusLossPct_ 通道
//               （音频采集线程）—— 原子交接，各自应用
//
// 【防乒乓：快降慢升】
//   降级 3 tick（600ms）生效，升级 10 tick（2s）生效。
//   降级快：网络恶化每 600ms 都在持续伤体验，晚降级 = 持续卡顿
//   升级慢：带宽恢复初期不稳（WiFi 漫游回切/拥塞缓释），立刻升回
//           容易再次打爆带宽来回震荡。不对称是 AIMD 同款哲学。
//
// 【帧率阶梯】
//   {100%, 2/3, 50%, 40%, 33%} × 配置帧率，下限 10fps：
//   降级直接跳档（Poor→50% 档，Bad→下限），升级每级至少停 2s
//   （minDwell）逐级爬。降帧率不重配编码器（x264 上下文不动），
//   对齐 libwebrtc MaintainResolution 的 frame dropping 路径。
//
//【FEC 上限曲线（protection overhead）】
//   Good/Fair 40% / Poor 50% / Bad 30%：
//   Poor 放宽——丢包率真实升高，需要更多冗余；
//   Bad 收紧——链路极度受限时，过度 FEC 会挤占视频有效载荷，
//   保护性开销的带宽分配是真实工程的经典权衡。
//
// 【PLI edge 语义】
//   进入 Bad 瞬间 requestPli=true（一次），消费即清。Bad 期间不重复
//   请求（发送侧 sendPli 已有 500ms 节流，双保险防关键帧风暴）。
//
//【线程模型】
//   仅主循环 200ms tick 调用，无锁；决策经 atomic/值拷贝交接执行器。
// ============================================================================
#pragma once

#include "media/adaptive/network_quality_classifier.h"
#include <cstdint>
#include <vector>

namespace crystal {

// 一次 tick 的决策输出（值语义返回，调用方自行交接）
struct AdaptationDecision {
    NetworkQuality level = NetworkQuality::Good;
    uint32_t targetFps = 30;     // 编码线程目标帧率
    uint32_t fecCapPct = 40;     // Opus FEC 冗余度上限（%）
    bool requestPli = false;     // edge：本次 tick 进入 Bad 时为 true
    uint64_t levelAgeMs = 0;     // 当前等级已持续时间（观测用）
};

class AdaptationController {
public:
    // configuredKbps - 编码器初始码率（ratio 分母）
    // configuredFps  - 编码器配置帧率（阶梯基準）
    AdaptationController(uint32_t configuredKbps, uint32_t configuredFps);

    // 主循环每 200ms 驱动：分级 → 去抖 → 阶梯 → 组装决策
    AdaptationDecision tick(const NetworkSignals& signals, uint64_t nowMs);

    // 最近一次决策（[net] 行随 5s 周期读取）
    const AdaptationDecision& lastDecision() const { return decision_; }

    static constexpr uint32_t kMinFps = 10;        // 帧率下限
    static constexpr int kDowngradeTicks = 3;      // 降级去抖（600ms）
    static constexpr int kUpgradeTicks = 10;       // 升级去抖（2s）
    static constexpr uint64_t kMinDwellMs = 2000;  // 帧率档位最小停留

private:
    // 帧率阶梯：降级跳档 / 升级逐级（受 minDwell 约束）
    void applyLadder(NetworkQuality level, uint64_t nowMs);
    size_t currentRung() const;
    static std::vector<uint32_t> buildLadder(uint32_t configuredFps);
    static uint32_t fecCapFor(NetworkQuality q);

    uint32_t configuredKbps_;
    uint32_t configuredFps_;
    std::vector<uint32_t> ladder_;      // 降序：ladder_[0]=配置帧率 … back()=下限

    // --- 去抖状态机 ---
    NetworkQuality level_ = NetworkQuality::Good;     // 当前生效等级
    NetworkQuality candidate_ = NetworkQuality::Good; // 正在累计的候选
    int candidateStreak_ = 0;                        // 候选连续次数
    uint64_t levelSinceMs_ = 0;                       // 本级生效起始

    // --- 阶梯状态 ---
    uint32_t targetFps_;
    uint64_t lastLadderMoveMs_ = 0;                   // 上次移档时刻

    bool pliPending_ = false;                          // 进入 Bad 置位
    AdaptationDecision decision_;
};

} // namespace crystal
