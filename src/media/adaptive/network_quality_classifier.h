// ============================================================================
// network_quality_classifier.h - 网络分级器（工程化升级 v2/Phase E 新增）
// ============================================================================
//
// 【在 WebRTC 系统中的角色】
// GCC 回答"带宽是多少"，本组件回答"网络处于什么状态"——后者是跨流
// 策略（降帧率/FEC 上限/PLI）的输入。对齐 libwebrtc 里 Call 级
// NetworkController 的信号汇聚角色（简化为单函数分级）。
//
// 【纯函数设计（可测性）】
// 无时钟、无锁、无副作用：输入信号结构体，输出等级。所有阈值是
// 常量，单测直接构造边界值验证。真实工程的阈值随场景调参
// （会议/直播/1v1 各不同），本实现取 WebRTC 通话的典型量级。
//
// 【为什么用级联而不是独立区间】
// 阈值区间重叠时（如 ratio=0.5 且 loss=20% 同时落 Poor 与 Bad 的
// 字面条件），级联判定（Good→Fair→Bad，命中即返回，兜底 Poor）
// 保证结果唯一——这是状态机防二义性的标准写法。
//
// 【信号说明】
// - ratio = gccTargetKbps / configuredKbps：可用带宽相对编码器期望的
//   比例。GCC 收敛后 ratio<1 说明带宽撑不起当前配置，是降级的核心理由
// - rttMs < 0 表示未知（SR/RR 还没配对出 RTT）：该条件视为不命中，
//   不阻塞判级也不误触发 Bad
// - jitterMs/freezeCount 本阶段仅随 [net] 行观测输出，不参与判级
//   （链路信号与体验信号混判容易互斥打架，见指南"生产差异"）
// ============================================================================
#pragma once

#include <cstdint>

namespace crystal {

// 网络质量等级（数值序即劣化序：Good < Fair < Poor < Bad）
enum class NetworkQuality { Good, Fair, Poor, Bad };

// 分级输入信号（主循环每 200ms 收集一次）
struct NetworkSignals {
    uint32_t gccTargetKbps = 1000;  // GCC 当前目标码率
    uint32_t configuredKbps = 1000; // 编码器初始配置码率（ratio 分母）
    double rttMs = -1.0;            // SR/RR 配对 RTT；<0 未知
    double lossPct = 0.0;          // 对端 RR 观测丢包率（百分比）
    double jitterMs = 0.0;         // 平滑抖动（仅观测）
    uint32_t freezeCount = 0;      // 渲染卡顿累计（仅观测）
};

// 级联判定：Good → Fair → Bad（命中即返回），兜底 Poor
NetworkQuality classifyNetwork(const NetworkSignals& s);

// 等级名（日志/[net] 行用）
const char* networkQualityName(NetworkQuality q);

} // namespace crystal
