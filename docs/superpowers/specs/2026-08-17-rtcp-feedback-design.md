# CrystalRTC RTCP 反馈体系设计（NACK + PLI + SR/RR）

- 日期：2026-08-17
- 状态：已评审通过
- 前置：Phase 1（P2P 音视频通话）已完成
- 关联：`2026-05-19-crystal-rtc-design.md`（本设计对应其 Phase 3 中"RTCP/NACK/PLI/统计"部分的提前实现）

## 1. 目标与范围

为 CrystalRTC 的 P2P 音视频通话增加完整的 RTCP 反馈体系，解决 Phase 1 的核心弱点：
弱网丢包时画面花屏无法恢复、无质量指标可观测。

**本次实现**：

| 功能 | 说明 |
|------|------|
| NACK 丢包重传 | 接收端检测序列号间隙后请求对端补发，发送端从重传缓冲区响应（仅视频） |
| PLI 关键帧请求 | 接收端解码失败时请求对端立即编码关键帧，加速画面恢复（仅视频） |
| SR/RR 质量统计 | 双端周期交换 Sender/Receiver Report，输出丢包率、RTT、抖动、重传计数 |

**明确不做（面试考点，需能说出理由）**：

| 排除项 | 理由 |
|--------|------|
| 音频 NACK | Opus 帧 20ms，重传到达已过播放时刻，标准实践靠 FEC/丢包隐藏（PLC） |
| REMB / 码率自适应 | 作为下一独立阶段（见设计文档 Phase 2/3 规划） |
| RTX（RFC 4588） | 重传直接复用原序列号与原字节即可，不引入额外 SSRC 与 payload type 复杂度 |

**验收标准**：

1. `tc netem` 模拟 10% 丢包时，视频花屏/卡顿肉眼可见改善（NACK 生效）
2. 解码错误后一个 GOP 周期（约 1s，30fps/GOP30）内画面恢复（PLI 生效）
3. 双端终端每 5s 打印一行质量统计（丢包率 / RTT / 抖动 / 重传次数 / NACK 未命中数）
4. 浏览器互通不回退：Chrome 发来的 NACK / PLI 能被正确响应，Chrome 也能响应我方请求
5. 新增单元测试全部通过

## 2. 架构决策记录

### 2.1 实现深度：全自实现

libdatachannel v0.21.2 内置了 `RtcpNackResponder`、`RtcpSrReporter`、`Track::requestKeyframe()`
（发送 PLI）等能力，但本项目选择**自实现全部 RTCP 报文与策略逻辑**，libdatachannel 仅作为
"加密字节搬运工"。理由：

- 本项目定位是学习型/面试展示项目，核心价值在自实现协议细节
- 与现有 `rtp_packet` / `rtp_packetizer` / `jitter_buffer` 的自实现风格一致
- 面试可分层讲述协议编解码、丢包重传策略、统计计算等底层细节

**libdatachannel 能力边界（已核实 v0.21.2 源码）**：

- 接收方向：`Track::onMessage` 回调会同时收到 RTP 与 RTCP（RTCP 以控制消息形式入队），
  应用层按 RFC 5761 规则去复用即可，无需 MediaHandler 链
- 发送方向：`track->send(rtcp字节)` 可直接发送原始 RTCP（库内部按 `IsRtcp` 判定标记为控制包）

### 2.2 模块组织：协议层与策略层分离

```
src/media/rtcp/                        ← 新模块，与 src/media/rtp/ 平级
├── rtcp_packet.h/cpp                  协议层：纯字节构造/解析，无状态，可全量单测
├── retransmission_buffer.h/cpp        发送端策略：重传缓冲
├── nack_requester.h/cpp               接收端策略：NACK 请求状态机
└── rtcp_reporter.h/cpp                统计：SR/RR 周期收发与指标计算
```

备选方案（已否决）：
- 单一 `RtcpSession` 大类：职责混杂、难单测、面试讲述分层感差
- 继承 `rtc::MediaHandler` 挂链：策略逻辑绑死在库的回调模型上，自实现纯度降低

## 3. 模块详细设计

### 3.1 协议层 `rtcp_packet`

支持 RFC 3550 / RFC 4585 报文子集：

| 类型 | PT/FMT | 内容 |
|------|--------|------|
| SR（Sender Report） | PT=200 | SSRC、NTP 时间戳（64bit）、RTP 时间戳、发包数、字节数、report block(s) |
| RR（Receiver Report） | PT=201 | SSRC、report block(s)（fraction lost 8bit / cumulative lost 24bit / ext highest seq / jitter / LSR / DLSR） |
| SDES | PT=202 | 最小实现：仅 CNAME 项（SSRC 的十六进制），复合包携带 |
| NACK | PT=205, FMT=1（RTPFB） | sender SSRC、media SSRC、FCI 列表（PID 16bit + BLP 16bit 位图） |
| PLI | PT=206, FMT=1（PSFB） | sender SSRC、media SSRC |

接口形态：

- 每种类型提供 POD 结构体 + `serialize()`（追加到字节缓冲）与 `parse()`（从字节切片还原）
- `RtcpCompoundParser`：迭代解析复合包（一个 UDP 载荷含多个子包）
- `RtcpCompoundBuilder`：拼装复合包（如 RR + SDES）
- NACK 的 FCI 生成辅助：输入排序去重后的丢失 seq 列表，按"PID + BLP 位图"合并
  （一个 FCI 表达 17 个连续序列号：PID 本身 + BLP 的 16 个位）

### 3.2 发送端策略 `RetransmissionBuffer`

- 环形缓冲：容量 512 包，另按时间淘汰（超过 3s 的包可回收）
- `store(seq, bytes)`：视频 RTP 包发出前调用
- `onNack(seqs)`：命中则按原序列号、原字节补发；未命中（已淘汰）计入 miss 统计
- 统计输出：重传次数、NACK 未命中数
- 线程模型：仅媒体发送线程访问，无锁（与现有发送路径一致）

### 3.3 接收端策略 `NackRequester`

- 输入：`JitterBuffer::insert()` 检测到的新间隙序列号列表
- 状态表：`seq → {请求次数, 上次请求时间}`
- 策略：
  - 新间隙立即生成 NACK 请求
  - 未恢复的 seq 重试，上限 3 次，间隔 33ms（约一个视频帧周期）
  - 超过上限放弃（大概率真丢包，交给 PLI 兜底恢复画面）
- 输出：到期需要请求的 seq 集合，由上层合成为 NACK 报文发送
- 驱动方式：新间隙由 `insert()` 调用时同步上报；重试到期由上层周期驱动
  （`main_client` 的接收循环中每帧处理时调用 `tick(now)`，取出到期 seq）

### 3.4 统计 `RtcpReporter`

- 发送侧（每流一个实例）：
  - 周期 5s 构造并发送 SR；维护会话起点做 NTP↔RTP 时间换算
  - 收到对端 RR 时计算 RTT：`RTT = NTP_now - LSR - DLSR`（RFC 3550 A.6）
- 接收侧（每流一个实例）：
  - 每个到达的 RTP 包按 RFC 3550 A.8 更新到达间隔抖动（interarrival jitter）
  - 周期 5s 构造并发送 RR（含 report block）+ SDES CNAME 复合包
- `Stats` 结构：fraction lost、cumulative lost、jitter(ms)、rtt(ms)、重传相关计数，
  供终端周期打印（也作为未来码率自适应的输入接口）

### 3.5 现有代码的最小侵入改动

| 文件 | 改动 |
|------|------|
| `transport_manager.h/cpp` | `PeerConnection` 内部按 RFC 5761 去复用（首字节低 7 位 ∈ [192,223] 判为 RTCP）：RTP 走现有 `onTrack` 不变；新增 `onRtcp` 回调与 `sendRtcp(bytes)` |
| `jitter_buffer.h/cpp` | `insert()` 返回 `std::vector<uint16_t>`（本次新检测到的丢失 seq）；现有调用方忽略返回值不破坏兼容 |
| `h264_encoder.h/cpp` | 新增 `forceKeyframe()`：置标志，下一帧强制 IDR |
| `h264_decoder.h/cpp` | 新增 `onDecoderError` 回调：FFmpeg 返回错误或未产出帧时触发 |
| `rtp_packetizer.*` | SSRC 由写死值改为随机生成（修复 Phase 1 遗留：两端同 SSRC 导致 RTCP 统计无法归属） |
| SDP | 视频 m-line 补 `a=rtcp-fb:96 nack` 与 `a=rtcp-fb:96 nack pli`；实施时优先查 `Description::Media` 相关 API，兜底方案为 SDP 字符串后处理 |

### 3.6 集成接线（`main_client.cpp`）

```
接收端:
track.onMessage ─→ PeerConnection demux ─┬ RTP → onTrack → 解析 → JitterBuffer.insert
                                          │                    └ 新间隙seq → NackRequester
                                          │                         └ 到期未恢复 → NACK → sendRtcp
                                          └ RTCP → onRtcp → 复合包解析:
                                                ├ NACK → RetransmissionBuffer 补发原包
                                                ├ PLI  → H264Encoder.forceKeyframe()
                                                ├ RR   → Reporter 更新对端观测(丢包率/RTT)
                                                └ SR   → 记录同步信息
解码失败 → 节流500ms → PLI → sendRtcp

发送端: packetizer出包 → RetransmissionBuffer.store → track.send
统计:   Reporter每5s发SR(音视频各一) + RR，终端打印一行
```

PLI 触发源：解码错误（节流 500ms）。
"高丢包率触发 PLI"（fraction lost 超阈值时主动请求关键帧）列为可选增强，不在本次范围。

## 4. 关键参数

| 参数 | 值 | 理由 |
|------|-----|------|
| NACK 重试上限 | 3 次 | 超过说明包大概率真丢，继续等待不如让 PLI 兜底恢复 |
| 重传缓冲容量 | 512 包 / 3s 时间淘汰 | ≈2-6s 视频流量，覆盖正常 RTT 下的 NACK 请求窗口 |
| SR/RR 周期 | 5s | RFC 3550 推荐值 |
| PLI 节流 | 500ms | 防止解码错误风暴触发关键帧风暴（关键帧大，会挤占带宽） |

## 5. 错误处理

| 场景 | 处理 |
|------|------|
| 复合包中单个子包畸形（长度不齐/PT 非法） | 丢弃该子包，继续解析下一个 |
| NACK 请求的包已不在重传缓冲 | 计入 miss 统计后放弃 |
| RTCP 报文 SSRC 不匹配本地流 | 忽略该报文 |
| track 未就绪时发送 RTCP | 静默丢弃（连接状态机已有处理） |

## 6. 测试策略

新增测试文件（独立 `crystal_rtcp_tests` 可执行目标，链接 `crystal_media_rtcp`）：

| 文件 | 覆盖 |
|------|------|
| `rtcp_packet_test.cpp` | SR/RR/NACK/PLI 构造→解析 round-trip 字段一致；PID+BLP 位图合并正确；复合包迭代；畸形包安全拒绝 |
| `retransmission_buffer_test.cpp` | 命中补发字节一致；超窗未命中；容量淘汰最老包 |
| `nack_requester_test.cpp` | 新间隙立即请求；重试上限 3 次；恢复后不再请求 |
| `rtcp_reporter_test.cpp` | RR 解析后统计更新；RTT 计算（注入时钟）；抖动更新 |

集成验证：`tc netem` 弱网脚本（模拟 10%/20% 丢包）+ 手测步骤写入 `USAGE.md`。

## 7. 文档同步计划

| 文档 | 改动 |
|------|------|
| `docs/superpowers/specs/2026-08-17-rtcp-feedback-design.md` | 本设计文档（新建） |
| `docs/LEARNING_GUIDE.md` | 新增 Module 8：RTCP 与弱网对抗（概念讲解、代码走读、动手练习、面试高频题） |
| `docs/USAGE.md` | 新增质量统计日志说明、`tc netem` 弱网测试方法 |
| `README.md` | 特性表补充"RTCP 反馈体系（NACK/PLI/SR-RR）" |
| `src/CMakeLists.txt`、`tests/CMakeLists.txt` | 新增 `crystal_media_rtcp` 库与测试源文件 |
| 代码注释 | 新模块沿用项目详尽中文注释风格（协议字段图、算法流程、面试要点） |

## 8. 面试知识点映射（本设计的讲法）

- 为什么音频不做 NACK / 视频做（延迟敏感性差异 + FEC/PLC 机制）
- NACK vs PLI 的适用场景（单包丢失 vs 参考帧链断裂）
- PID+BLP 位图如何用一个 FCI 表达 17 个连续丢失包（RTCP 开销优化）
- RTT 为什么只能在发送侧算（需要 LSR/DLSR 配合本机时钟）
- 重传缓冲为什么按"包数 + 时间"双维度淘汰
- PLI 为什么需要节流（关键帧码率尖峰与风暴问题）
- RFC 5761 的 RTP/RTCP 复用去复用规则（首字节 PT 区间）
