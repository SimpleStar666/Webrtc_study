# Phase D：延迟与线程（设计文档 5.1–5.4）

对应设计文档：`docs/superpowers/specs/2026-09-09-crystalrtc-v2-engineering-upgrade-design.md` 第 5 节。

## 目标

1. JitterBuffer 从"纯序号启发式"升级为"时间驱动释放"（自适应生效延迟）
2. SDLAudioPlayer 从 mutex+queue 升级为无锁 SPSC 环形缓冲（实时回调不能持锁）
3. 视频发送链路三级解耦：采集回调只 memcpy 入队，编码线程做 encode→packetize→send
4. 顺带：RetransmissionBuffer::get() O(1) 索引
5. 修复既有跨线程竞争（见"线程安全审计"）

## Task 1：SPSC 环形缓冲（新文件 src/utils/spsc_ring.h）

- header-only 模板 `SpscRing<T>`：固定容量，`std::atomic<uint64_t>` 读写计数器
  （递增计数器取模定位槽位，永不回绕，无 ABA 问题）
- 内存序：写方 `store(release)` / 读方 `load(acquire)` 对称配对；
  `alignas(64)` 填充避免伪共享；溢出计数仅写方访问（relaxed）
- 批量 `push(const T*, n)`（满则丢最旧并计数）、批量 `pop(T*, n)`（返回实际数）
- 查询：`size()`（近似水位）、`overflowCount()`

## Task 2：JitterBuffer 时间驱动释放

- `RtpPacket` 增加 `arrivalMs`（运行时元数据，不参与序列化；调用方注入，类内不读时钟→可测）
- `consume(uint64_t nowMs)` 新签名，输出条件改为：
  - 无缺口 → 输出（原逻辑）
  - 有缺口且 `nowMs - 队首到达 ≥ effectiveDelay` → 跳过缺口输出（新能力）
  - 有缺口且等待不足 → 继续等
  - `nowMs == 0` 退化为旧规则（diff≤3 立即输出，向后兼容存量测试）
- 自适应生效延迟：`clamp(基础延迟 + 乱序深度 × 包间隔, 基础延迟, 200)`
  - 乱序深度 = 迟到包序号跨度 EMA；包间隔 = 相邻到达间隔 EMA
  - 构造参数 targetDelayMs 语义升级为"基础延迟"（接口不变）
- 暴露 `effectiveDelayMs()` 供测试与观测

## Task 3：SDLAudioPlayer 改造（SPSC + 指标）

- `std::queue<int16_t>` + mutex → `SpscRing<int16_t>`（容量 500ms @48kHz 单声道）
- `play()` 无锁写入；`fillBuffer()` 无锁读出 + 不足填静音
- 新增观测：`bufferedMs()` / `underflowCount()` / `overflowCount()`
- main_client `[metrics][a]` 行追加播放缓冲水位与下溢计数

## Task 4：视频发送三级解耦（main_client.cpp）

- `SpscRing<std::vector<uint8_t>>`（容量 2）帧队列，满丢最旧（丢帧计数）
- 采集回调：只做 memcpy 入队
- 新编码线程：pop → `encoder.encode()`；onEncoded 回调链（packetize→send→
  retxBuffer→twccSendTimes→metrics→reporter）随之迁移到编码线程
- GCC 码率应用改原子交接：主循环 `gccTargetKbps_.store()`，编码线程每帧前
  读取并 `encoder.setBitrate()`（消除 FFmpeg 上下文跨线程并发）

## 线程安全审计（本阶段修复的既有竞争）

| 共享状态 | 写线程 | 读/写线程 | 修复 |
|---|---|---|---|
| twccSendTimes_ (map) | 编码线程 | datachannel（RTCP） | main_client 加 mutex |
| GccController | datachannel(onFeedback/onLoss) | 主循环(tick) | 类内加 mutex |
| TwccRecorder | datachannel(onPacket) | 主循环(buildFeedback) | 类内加 mutex |
| encoder.setBitrate | 主循环 | 编码线程(encode) | 原子目标 + 编码线程应用 |
| opusEncoder ctl | 主循环 | ALSA 线程(encode) | 原子目标 + 采集线程应用 |
| videoPacketizer | — | 仅编码线程使用 | 无需改（单线程独占） |

SendSideReporter / MetricsCollector / RetransmissionBuffer / NackRequester 已有锁，不动。

## Task 5：RetransmissionBuffer O(1)

- `deque<Entry>` 线性扫描 → `unordered_map<uint16_t, Entry>` + `deque<uint16_t>` 淘汰序

## Task 6：测试 + 文档

- `tests/spsc_ring_test.cpp`：单线程往返 / 溢出丢最旧 / 两线程压测零丢失零重复（新目标 crystal_thread_tests）
- `tests/jitter_time_test.cpp`：等待不足不释放、等够释放、自适应延迟加深、无参 consume 向后兼容（并入 crystal_rtp_tests）
- LEARNING_GUIDE.md：Module 12 线程模型与无锁编程；Module 3 时间驱动释放小节【v2 新增】；
  Module 0 线程拓扑图三级解耦；面试专题线程安全追问题；TOC
- 全量构建 + ctest 全绿 + demo 不回归

## 验收

- 新旧测试全绿；demo 1–5 输出不回归
- [metrics][a] 行可见播放缓冲水位/下溢计数
- 编码线程的丢帧计数（frameQueue 溢出）可观测
