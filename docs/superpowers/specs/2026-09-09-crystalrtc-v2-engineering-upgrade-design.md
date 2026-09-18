# CrystalRTC 工程化升级 v2 设计文档

日期：2026-09-09
状态：已确认（用户委托按真实工程惯例决策，LEARNING_GUIDE.md 中详细讲解每个决策）

## 1. 目标与背景

**用户目标**：求职音视频客户端/终端 SDK 岗位，需要项目达到"入职能直接上手"的程度。

**现状问题**（审计结论）：项目存在 6 类简化实现——
1. 码率写死 1000kbps，无带宽估计（无 TWCC/REMB），无自适应码率闭环（ABR）
2. JitterBuffer 的 targetDelayMs_(40) 只存不用，无时间驱动释放
3. 音频播放 std::queue<int16_t> 逐采样入队，无环形缓冲
4. 编码在采集回调线程同步执行（采集→编码→打包→发送全串行）
5. Opus 的 inband FEC、DTX 均未开启
6. 无可观测性指标（卡顿、端到端延迟、帧率、缓冲水位）

**总原则**：所有技术决策对齐 libwebrtc 生产实现路线（GCC/Trendline 而非已弃用的 REMB；TWCC 而非单纯丢包率），自研实现保持与现有自研 RTP/RTCP 栈风格一致（面试价值最大化）。

## 2. 总体顺序

```
Phase A 可观测性 → Phase B Opus FEC/DTX → Phase C 延迟+线程 → Phase D 完整 GCC
```

理由：
- A 先行：后续每个 Phase 的效果需要用数据验证（GCC 收敛曲线、FEC 恢复率、卡顿下降）
- C 在 D 之前：GCC 要动态调编码器码率，且发送路径要插 TWCC 头，先把线程结构改稳定，避免 main_client.cpp 大改两次
- D 最后：工作量最大（约一半），专注做

## 3. Phase A：可观测性（新增 src/media/monitor/）

新增 `MetricsCollector`（每条流一个实例），主循环每 5s 与现有 [stats] 一起输出结构化 [metrics] 行：

| 指标 | 算法 | 备注 |
|------|------|------|
| 卡顿次数 | 渲染帧间隔 > 1.5×预期帧距（30fps 即 >50ms）累计 | 渲染回调处打点 |
| 实际渲染帧率 | 5s 滑动窗口渲染帧数 | |
| 端到端延迟 | 对端 SR 的 NTP↔RTP 映射 + 本地接收时刻 − RTT/2 | libwebrtc 同款思路，双端时钟不同步也成立 |
| 音频缓冲水位/下溢次数 | 环形缓冲填充率 | Phase C 环形缓冲完成后接入 |
| 发送码率 | 5s 滑动窗口字节/秒 | |

文件：`metrics_collector.h/.cpp`（含中文教学注释），`src/CMakeLists.txt` 归入 crystal_media 或新建 crystal_media_monitor。

## 4. Phase B：Opus FEC/DTX（音频弱网对抗）

**编码端**：
- `OpusEncoder::init()`：`OPUS_SET_INBAND_FEC(1)`、`OPUS_SET_DTX(1)`
- `OpusEncoder::setPacketLossPct(uint32_t)`：由接收端 RR 实测丢包率动态喂给 `OPUS_SET_PACKET_LOSS_PCT`（工程真实做法：FEC 冗余度随实测丢包动态调整）

**解码端（关键工程点）**：
- `OpusDecoder::decode()` 增加"带后续帧解码"模式：JitterBuffer 输出发现 seq 缺口时，把**下一个收到的包**传给 `opus_decode`（FEC 精确恢复上一帧），而非传 NULL（NULL 只触发 PLC 模糊推测）
- PLC 与 FEC 的效果对比在 Demo 中演示

**验证**：人为丢包下对比 PLC-only 与 FEC 恢复帧；静音时观察 DTX 1~2 字节舒适噪声帧。

## 5. Phase C：延迟与线程

### 5.1 JitterBuffer 时间驱动释放
- `RtpPacket` 增加 `arrivalMs` 字段（insert 时打点）
- `consume(uint64_t nowMs)`：输出条件从纯序号改为"无缺口，或距到达已等待 ≥ 生效延迟"
- 生效延迟自适应：`clamp(40 + 乱序深度×包间隔, 40, 200)` ms——乱序越严重缓冲越深，这是真实播放时钟概念的落地
- 保持向后兼容：constructor 的 targetDelayMs 语义升级为"基础延迟"

### 5.2 SPSC 环形音频缓冲
- `SDLAudioPlayer`：`std::queue<int16_t>` → 固定容量无锁环形缓冲（std::atomic 读写指针；SDL 回调线程消费、解码线程生产）
- 溢出丢最旧（低延迟优先）；水位与下溢计数接入 Phase A 指标

### 5.3 视频发送链路解耦
- 新增编码线程 + 帧队列（容量 2 的 SPSC，满则丢最旧并计数）
- 采集回调线程只做 memcpy 入队，编码线程做 encode→packetize→send
- **不做 pacer**（平滑发送），写入指南"生产差异"章节
- 音频路径保持现结构（负载轻），指南说明真实工程同样会解耦

### 5.4 顺带小优化（不单列验收）
- `RetransmissionBuffer::get()` 加 unordered_map<seq, index> 辅助索引（注释中承诺过的 O(1)）

## 6. Phase D：完整 GCC（五个新组件，自底向上）

```
发送端                                        接收端
每个RTP包打TWCC扩展头(0xBEDE,2字节序号) ──→ 记录(seq,到达时刻)，每100ms
                                              打RTCP TransportFeedback包 ──→
Trendline滤波(OWD一阶差分线性回归,20样本窗) ←── 解码feedback还原样本
        ↓
过载检测状态机(NORMAL/UNDERUSE/OVERUSE)  ┐
丢包率通道(>10%强降/<2%交棒趋势通道)     ├─→ AIMD速率控制器(×0.85降/递增)
                                         ↓      目标码率(平滑,最小5%变动,钳[100,4000]kbps)
                          H264Encoder::setBitrate() 运行时调码率
```

### 6.1 新组件清单
| 组件 | 文件 | 说明 |
|------|------|------|
| TWCC 扩展头 | rtp_packet.h/.cpp 扩展 | one-byte extension（0xBEDE + ID=1 + 2 字节递增序号）；serialize/parse 双向支持，这是现有 RtpPacket 的真缺口 |
| TransportFeedback 包 | src/media/rtcp/transport_feedback.h/.cpp | RTCP PT=205/FMT=15，Google 格式（base PID + symbol vector + receive delta 编码），自研编解码 |
| 远端统计收集 | twcc_recorder.h/.cpp | 接收端记录 (seq, 到达时刻)，100ms 打包 feedback |
| Trendline 估计器 | trendline_estimator.h/.cpp | libwebrtc 同款简化线性回归 |
| 过载检测+AIMD | gcc_controller.h/.cpp | 双通道融合（丢包通道+趋势通道），状态机+速率控制器合一 |

### 6.2 编码器动态调码率
- `H264Encoder::setBitrate(uint32_t kbps)`：运行时更新 `codecCtx_->bit_rate`，钳制 [100, 4000]
- libx264 运行时改 bit_rate 的注意点（需重置 RC 或接受渐变）写入指南

### 6.3 验证
- `main_demo.cpp` 新增 Demo 5：虚拟信道模拟带宽变化（无硬件验证 GCC）
- `scripts/netem-bandwidth.sh`：tc tbf+netem 限速脚本，实测"带宽 2M 砍 500k，码率 3s 内收敛"
- GCC 面试话术与实测曲线写入 LEARNING_GUIDE.md 面试专题

## 7. LEARNING_GUIDE.md 同步更新（核心要求）

1. **所有新增内容统一打【工程化升级 v2 新增】标记**，旧内容不删（保留学习曲线）
2. 新增两个完整模块：
   - Module 11：GCC 带宽估计与自适应码率（原理图 → 逐组件精读 → 弱网实测 → 面试题，含"为什么 WebRTC 弃用 REMB""Trendline 为什么用斜率不用原始 OWD"）
   - Module 12：线程模型与无锁编程（SPSC 原理、内存序、为什么 SDL 回调里不能加锁、采集/编码/渲染线程拓扑图）
3. 已有模块插入标记块：
   - Module 3：时间驱动释放小节（【v2 新增】）
   - Module 5：FEC/DTX 实战小节
   - Module 0：线程模型图更新（三级解耦后）
   - 面试专题：GCC 与线程安全追问题
4. 每个 Phase 附"生产差异"说明：没做的（pacer、probe 探测、SVC/Simulcast、保护性开销分配），真实工程怎么做、为什么本阶段不做

## 8. 测试策略

每组件 googletest 单测（tests/ 下新增，随 Phase 提交）：
- Phase A：MetricsCollector 时间窗口打点
- Phase B：FEC 恢复对比（构造丢包序列）、DTX 帧大小
- Phase C：时间驱动释放（注入时间序列的乱序/迟到）、SPSC ring 并发压测、三级解耦后端到端 demo 不回归
- Phase D：TWCC 头回绕 round-trip、TransportFeedback 编解码 round-trip 与回绕、Trendline 三组造数据斜率（增长/平稳/下降）、AIMD 状态迁移、setBitrate 钳制

## 9. 明确不做（out of scope）

- Pacer（平滑发送）——指南"生产差异"说明
- Probe（带宽探测）——同上
- SVC/Simulcast、视频 FEC（FlexFEC/UlpFEC）
- 信令心跳/重连、SDP PT 写死改协商式
- SFU/服务端方向

## 10. 提交策略

按 Phase 分四次提交（A/B/C/D 各一），每次提交包含：代码 + 单测 + LEARNING_GUIDE.md 对应章节 + 注释（老师级中文注释，与现有风格一致）。提交信息用 conventional commits（feat/perf/docs）。
