# CrystalRTC Phase E 设计文档：网络自适应（AdaptationController 分层）

日期：2026-09-18
状态：已确认（用户选定：网络自适应为主，ICE 侧仅做连接状态监控；视频降级 = 码率 + 帧率钳制；架构 = 独立控制器）
前置：v2 设计文档（Phase A–D）已全部交付。

## 1. 目标与背景

v2 升级完成后，项目已有 GCC 带宽估计（动态调码率）、Opus FEC/DTX（随 RR 丢包率调冗余度）、
可观测性指标与三级线程解耦。但各执行器之间**没有统一的网络状态视图与跨流策略**：

- 视频 GCC 与音频 FEC 各自为政：网络恶化时视频在降码率，音频可能还在升 FEC 冗余
- 帧率不参与降级：带宽腰斩时仍按配置帧率编码，单帧质量被压到不可看
- 连接状态（disconnected/failed）不可观测，主循环不感知传输层健康度

Phase E 补上 libwebrtc 架构中 Call 级的一层：**网络状态分级 + 跨流自适应策略**。
对齐真实工程的职责边界：GCC 管带宽估计，Adaptation 层管跨流策略（降级偏好、保护开销、
关键帧恢复），ICE 栈继续交给 libdatachannel。

**面试价值主线**：degradation preference（为什么丢帧而非重配编码器）、降级快升级慢的
防乒乓设计、protection overhead（FEC 冗余度为什么有上限）、分级去抖。

## 2. 范围

### 做
1. `NetworkQualityClassifier`：纯函数分级器（GCC 码率压力 + RTT + 丢包率 → 四级网络等级）
2. `AdaptationController`：命令式策略层（决策表 → 帧率钳制 / FEC 上限 / PLI 关键帧）
3. `FramerateThrottler`：编码线程策略性丢帧（不重配编码器）
4. ICE/连接状态监控：`[net]` 指标行 + 断线告警日志
5. Demo 6（虚拟网络序列演示分级与决策）
6. LEARNING_GUIDE.md Module 13 + Module 0 / 面试专题 / TOC 更新

### 不做（写入指南"生产差异"）
- ICE 协议栈自研（与 libdatachannel 冲突，无工程价值）
- 分辨率降级（需重开 x264 上下文，重配瞬间卡顿；libwebrtc 的对应路径是 ReconfigureEncoder）
- Pacer / 带宽探测 probe（v2 设计文档已声明）
- 断线重连 / ICE restart（v2 设计文档已声明）
- 选中候选对（selected candidate pair）展示：libjuice 不暴露，libwebrtc 可从 getStats 拿

## 3. 架构

```
主循环(60fps 轮询)
  │ 每 200ms 打包 NetworkSignals:
  │   gccTargetKbps / 编码器配置码率 → 带宽压力比 ratio
  │   rttMs / lossPct (RTCP SR/RR, 5s 更新, 采样最近值)
  │   抖动 / 卡顿计数 (MetricsCollector)
  ▼
AdaptationController ──→ NetworkQualityClassifier(纯函数, 无状态判定)
  │                            │
  │ 决策(状态机: 去抖+阶梯)      │ NetworkQuality {Good|Fair|Poor|Bad}
  ▼
  ├─ targetFps_ (atomic) ──→ 编码线程 FramerateThrottler.shouldEncode()
  │                           (策略性丢帧, 区别于 Phase D 的 backlog 被动丢帧)
  ├─ fecCapPct_  ─────────→ 钳制 opusLossPct_ = min(实测, cap) → Opus FEC
  ├─ PLI 请求    ─────────→ Bad 级: 接收方向对端发 PliPacket(复用 appendPli)
  └─ [net] 行    ─────────→ Logger: 连接状态 + 等级 + 当前策略 + 累计决策
```

分层原则：GCC 组件不动（对齐 libwebrtc 的标准 GCC），Adaptation 是它之上的策略消费者；
PeerConnection::state() 轮询（不挂回调，避免与主循环线程竞争）。

## 4. 组件设计

### 4.1 NetworkQualityClassifier（src/media/adaptive/network_quality_classifier.h/.cpp）

纯逻辑：无时钟、无锁、无副作用——输入信号结构体，输出等级。全量可单测。

```cpp
enum class NetworkQuality { Good, Fair, Poor, Bad };

struct NetworkSignals {
    uint32_t gccTargetKbps;     // GCC 当前目标码率
    uint32_t configuredKbps;    // 编码器初始配置码率（压力比的分母）
    double   rttMs;             // SR/RR 配对 RTT；<0 表示未知
    double   lossPct;           // 对端 RR 观测丢包率（百分比）
    double   jitterMs;          // 平滑抖动
    uint32_t freezeCount;       // 渲染卡顿累计（体验信号）
};

NetworkQuality classify(const NetworkSignals& s) const;  // 无状态单次判定
```

单次判定规则（初值，可调），**按 Good → Fair → Bad 顺序级联，命中即返回，
都不中为 Poor**（避免阈值区间重叠时的二义性）：

| 顺序 | 等级 | 条件 |
|---|---|---|
| 1 | Good | ratio ≥ 0.85 且 loss < 2% 且 rtt < 150ms |
| 2 | Fair | ratio ≥ 0.60 且 loss < 5% 且 rtt < 300ms |
| 3 | Bad | ratio < 0.35 或 loss ≥ 15% 或 rtt > 800ms（任一命中） |
| 4 | Poor | 其余（中度劣化：未达 Good/Fair，也未恶劣到 Bad） |

`ratio = gccTargetKbps / configuredKbps`。rtt 未知（<0）时不参与判级（该条视为不命中）。
freezeCount/jitterMs 本阶段只进 [net] 行观测，不参与判级（避免体验信号与链路信号
互斥打架，真实做法见指南"生产差异"）。

**去抖状态机（classify 之上的 update 层）**：
- 降级：连续 3 tick（600ms）判定为更差等级 → 切换（快降）
- 升级：连续 10 tick（2s）判定为更好等级 → 切换（慢升）
- 非连续变化不切换（防乒乓：码率在阈值附近震荡时等级保持）

### 4.2 AdaptationController（src/media/adaptive/adaptation_controller.h/.cpp）

持有 Classifier + 去抖状态 + 决策状态。`tick(NetworkSignals, nowMs)` 每 200ms 由主循环驱动。

决策表（初值）：

| 等级 | targetFps | FEC 钳制上限 | 附加动作 |
|---|---|---|---|
| Good/Fair | 升一级（受阶梯与 minDwell 约束） | 40% | 无 |
| Poor | 15 | 50% | 无（码率钳制由 GCC 通道完成） |
| Bad | 10（下限） | 30% | 发一次 PLI（对端→恢复本端解码）+ 等级进 [net] 行 |

帧率阶梯：`30 → 20 → 15 → 12 → 10`（下限）。
- 降级：Poor → 15（固定档），Bad → 10（下限）
- 升级：沿阶梯逐级恢复，每级至少停留 2s（`minDwellMs`），不允许 10→30 一步跳

输出（供 main_client 原子交接）：
```cpp
struct AdaptationDecision {
    NetworkQuality level;
    uint32_t targetFps;
    uint32_t fecCapPct;
    bool     requestPli;      // 消费一次即清（edge 语义）
    uint64_t levelChangedMs;  // 本级已持续时间
};
```

FEC 钳制曲线说明：Poor 放宽到 50%（丢包率真实升高，需要更多保护），Bad 收紧到 30%
（链路极度受限时过度 FEC 会挤占视频有效载荷——protection overhead 概念）。
实测值与上限取 min 后走既有 `opusLossPct_` 通道。

### 4.3 FramerateThrottler（src/media/adaptive/framerate_throttler.h，header-only）

编码线程执行器，策略性丢帧：

```cpp
bool shouldEncode(uint64_t nowMs);
// 内部：if (nowMs - lastEncodedMs_ < 1000 / targetFps_) { policyDropped_++; return false; }
```

- 目标帧率由编码线程从 `targetFps_`（atomic）自取
- 与 Phase D 的 backlog drop（被动，队列满）正交且都计数：
  `policyDropped`（策略丢帧）/ `backlogDropped`（队列溢出丢帧）分别暴露给 [net]/[stats] 行
- 编码线程内部顺序：pop → backlog drop（Phase D）→ policy throttle（本阶段）→ encode
- RTP 时间戳：沿用 Phase D 的跳帧补偿（`videoRtpTs += (1+skipped) × tsStep`），
  policy 丢帧与 backlog 丢帧统一计入 skipped

### 4.4 连接状态监控（main_client 集成，无新组件）

- 主循环 200ms 周期轮询 `pc->state()`（`rtc::PeerConnection::State`，PeerConnection 已有
  `state()` 接口）——轮询而非回调，避免 libdatachannel 内部线程与主循环的共享状态
- Disconnected / Failed 时 Logger::warn 告警（一次性，状态沿检测）
- `[net]` 行（随 5s stats 周期输出）：
  `state=connected level=Fair rtt=210ms loss=3.1% | fps策略 15/30 fecCap 50% | 策略丢帧 8 队列丢帧 2`

## 5. main_client 集成

1. 构造 `AdaptationController`（配置码率用 `encConfig.bitrateKbps`，帧率用 `encConfig.fps`）
2. 主循环内独立 200ms 节流（与 5s stats 周期、100ms GCC tick 并列）：
   - 收集 NetworkSignals（gcc.targetBitrateKbps() / videoSendReport.rttMs() /
     remoteFractionLost / videoRecvReport.jitterMs() / videoMetrics freeze 计数）
   - `controller.tick(signals, nowMs)` → decision
   - `targetFps_.store(decision.targetFps)`（编码线程读）
   - `fecCapPct_` 保存，喂给既有 opusLossPct_ 通道前取 min
   - decision.requestPli 且接收轨道活跃 → 构造 PliPacket（media SSRC = 对端视频 SSRC）→ sendRtcp
3. [net] 行随 5s 周期输出（连接状态 + 等级 + 决策 + 计数）
4. 编码线程：pop 后先过 `FramerateThrottler::shouldEncode(now)`

线程安全：分级器无锁（仅主循环访问）；编码线程只读 atomic（targetFps_）；
PLI 在主循环发送（sendRtcp 线程安全，与既有 RR/SR 发送同路径）。

## 6. Demo 6：网络自适应（main_demo.cpp）

虚拟网络序列（无硬件验证）：
- 喂 60 tick（12s）的 NetworkSignals 时间线：Good 起步 → 带宽砍半（模拟 500k 限制）→
  丢包飙升（模拟 WiFi 切换）→ 恢复
- 打印每 tick：信号 → 分级（含去抖计数）→ 决策（fps/fecCap/PLI）
- 观察点：快降慢升（等级切换 tick 数差异）、帧率阶梯逐级恢复、Bad 级 PLI 触发一次

## 7. 测试策略

- `tests/network_classifier_test.cpp`（新目标 crystal_adaptive_tests）：
  - 四级边界判定（ratio/loss/rtt 各阈值正负）
  - rtt 未知不参与判级
  - 去抖：降级 3 tick 生效、2 tick 不生效；升级 10 tick 生效、9 tick 不生效
  - 防乒乓：等级在阈值附近震荡时保持
  - 帧率阶梯：Poor 跳半、Bad 落下限、升级逐级 + minDwell
  - FEC 钳制：三档 cap 值；PLI edge 语义（消费一次即清）
- `tests/framerate_throttler_test.cpp`：
  - 注入时间序列验证 30fps→15fps 丢帧节奏（该丢的丢、该编的编）
  - 恢复后回满帧率；policyDropped 计数正确
- 全量构建 + ctest 全绿 + demo 1–6 不回归

## 8. LEARNING_GUIDE.md 更新（核心要求）

1. 新增 **Module 13：网络自适应与降级策略**【工程化升级 v2 新增】
   - 分层图（GCC 管估计 / Adaptation 管策略 / 执行器）
   - 分级器阈值与去抖的工程权衡（为什么降级快、升级慢）
   - degradation preference 三模式对比（libwebrtc）与本项目的"丢帧不重配"取舍
   - protection overhead：FEC 冗余度上限曲线的带宽分配逻辑
   - 面试题：网络自适应追问题（升级为什么慢、帧率与分辨率降级的实现差异、
     FEC 与 NACK/RTX 的预算分配、无 RTT 时怎么分级）
2. Module 0：架构图加 AdaptationController 节点（信号→决策→执行器）
3. 面试专题：追加"网络自适应"追问链
4. TOC 更新
5. "生产差异"：libwebrtc 的 ResourceAdaptationProcessor / VideoStreamAdapter 与本实现的
   对应关系；selected candidate pair 的 getStats 视角；分辨率降级为何走 ReconfigureEncoder

## 9. 明确不做（out of scope）

见第 2 节"不做"清单。追加：跨流带宽分配（音视频预算拆分）、Simulcast/SVC、
基于 jitter 的分级输入（本阶段仅观测）。

## 10. 验收

- 单测全绿（新 crystal_adaptive_tests + 存量 6 套件不回归）
- demo 1–6 输出不回归，Demo 6 演示快降慢升与帧率阶梯
- 真机弱网（限速/丢包注入）时 [net] 行可见等级变化与 fps 策略切换
- 编码线程策略丢帧计数（policyDropped）可观测，与 backlog 丢帧分别计数
