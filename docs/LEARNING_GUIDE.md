# CrystalRTC 学习指南（老师级详解）

> 面向零基础到进阶学习者：本指南把 CrystalRTC 这个 C++ 自研 WebRTC P2P 音视频项目
> 拆成 11 个模块，用"先讲为什么、再读真代码、再动手改、最后背面试题"的方式带你吃透它。
> 全文所有代码引用均来自项目真实源码，可以打开对应文件逐行对照。

---

## 目录

- [如何使用本指南](#如何使用本指南)
- [Module 0：项目全景](#module-0项目全景)
- [Module 1：RTP 协议](#module-1rtp-协议)
- [Module 2：H.264 FU-A 打包](#module-2h264-fu-a-打包)
- [Module 3：Jitter Buffer](#module-3jitter-buffer)
- [Module 4：H.264 编解码](#module-4h264-编解码)
- [Module 5：Opus 音频编解码](#module-5opus-音频编解码)
- [Module 6：信令](#module-6信令)
- [Module 7：传输层 ICE/DTLS/SRTP](#module-7传输层-icedtlssrtp)
- [Module 8：RTCP 反馈体系](#module-8rtcp-反馈体系)
- [Module 9：采集与渲染](#module-9采集与渲染)
- [Module 10：工程化](#module-10工程化)
- [面试专题](#面试专题)
- [综合练习](#综合练习)
- [推荐阅读](#推荐阅读)

---

## 如何使用本指南

### 这份指南解决什么问题

市面上的 WebRTC 资料大多是两种极端：要么是浏览器 API 的使用手册（`new RTCPeerConnection()`
就完事），要么是 RFC 原文（晦涩、没有落地代码）。一个想"看懂本项目的代码、面试能讲清楚、
工作中能改得动"的初学者，最缺的是第三种：**把协议原理和一行行 C++ 代码对应起来**。

本指南就是第三种。它假设你已经会写基础的 C++（类、`std::vector`、`std::mutex`、回调
`std::function`），但对 WebRTC 的音视频传输几乎没有概念。我们从一个真实的、可编译运行的
自研项目出发，倒推每一个协议设计背后的"为什么"。

### 学习顺序

建议按下面的顺序学，不要跳：

```
0（项目全景）→ 1（RTP）→ 2（FU-A 分包）→ 3（Jitter Buffer）
   →（4 与 5 可并行：H.264 与 Opus）
   → 6（信令）→ 7（传输层）→ 8（RTCP）→ 9（采集渲染）→ 10（工程化）
   → 面试专题 → 综合练习
```

为什么是这个顺序？因为数据流的物理顺序是"采集 → 编码 → RTP → 网络 → 解 RTP → 解码 →
渲染"，但**理解顺序**要反过来：先理解 RTP 这个"通用信封"（Module 1），再理解信封里
装什么（Module 2 的 H.264 分片），再理解收信端怎么处理乱序和丢包（Module 3），之后才
分别讲视频（4）和音频（5）两种载荷如何编解码。信令（6）和传输层（7）是"旁路"和
"底层管道"，理解媒体格式后再看会轻松很多。RTCP（8）是控制面，贯穿所有模块，放在最后
能让你把前面的点串起来。

### 每个模块的四节结构

每个模块严格分为四节，**各有各的读法**：

| 小节 | 解决什么问题 | 怎么学 |
|------|--------------|--------|
| 概念讲解 | "这个模块在干什么、为什么这么设计" | 通读即可，重点看"为什么"，别背字段 |
| 代码精读 | "协议怎么变成 C++ 代码的" | 打开对应源文件，逐段对照阅读 |
| 动手练习 | "我到底懂没懂" | 改代码、跑 demo、观察输出 |
| 面试高频题 | "面试官会怎么问、我怎么答" | 先自己答一遍，再对答案要点 |

### 三条铁律

1. **先跑 demo，再读代码，再改代码。** 项目提供了无硬件依赖的演示程序 `crystal_demo`
   （`main_demo.cpp`），没有摄像头麦克风也能跑。先把四个 Demo 都跑一遍看到输出，再回头读源码。
2. **读码先读 `.h` 的头注释，再读 `.cpp` 的函数块注释。** 本项目源码注释非常详尽，
   几乎每个函数上方都有一大段中文注释解释"干什么、为什么、关键算法"。养成"先读注释、再读逻辑"的习惯。
3. **所有数值都要能手算。** 协议字段、丢包数、RTT、PID/BLP 位图……这些都能在纸上
   手算出结果。如果你能"手算一遍 RTP 头""手算一遍 RTT"，面试基本稳了。

---

## Module 0：项目全景

### 概念讲解

先建立全局观：CrystalRTC 是一个**自研的 WebRTC P2P 音视频通话**项目，两端（A、B）在
无中心媒体服务器的前提下，直接互相传输加密的音视频数据。

它跟"真正的 WebRTC 浏览器实现"最大的区别是：浏览器里的 WebRTC 是一套完整的、支持拥塞控制、
带宽估计、多路复用、抗丢包重传（RTX）等的工业级实现；本项目是一个**教学级实现**，把最核心
的那条媒体流水线"从采集到渲染"用 C++ 亲手写一遍，让你看清 WebRTC 的骨架而不是 API 黑盒。

#### 架构分层总览

整个系统可以画成下面这张数据流图（`main_client.cpp` 里也有同样的一张注释图）：

```
┌──────────────────────── 发送链路（本端 → 对端）────────────────────────┐
│                                                                        │
│   摄像头 ──[V4L2Capture]──> YUV ──[H264Encoder]──> NAL                │
│                                    ──[RtpPacketizer]──> RTP 包 ──┐    │
│   麦克风 ──[AlsaCapture]──> PCM ──[OpusEncoder]──> Opus 帧 ──┐    │    │
│                                    ──[RtpPacketizer]──> RTP 包 ──┤    │    │
│                                                                  ▼    │
│                               [PeerConnection]──(ICE/DTLS/SRTP)──> 网络 │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────────────── 接收链路（对端 → 本端）────────────────────────┐
│   网络 ──(SRTP 解密)──> [PeerConnection] ──RTP/RTCP 去复用──┐          │
│         │RTP                                          │RTCP          │
│         ▼                                             ▼              │
│   [JitterBuffer] 乱序重排+检测丢包              [RTCP 处理/NACK/PLI]   │
│         ▼                                                           │
│   [RtpDepacketizer] 解包（H.264 重组 FU-A / 取 Opus 帧）              │
│         ▼                                                           │
│   [H264Decoder]──> YUV ──[SDLRenderer]──> 屏幕                        │
│   [OpusDecoder]──> PCM ──[SDLAudioPlayer]──> 扬声器                   │
└────────────────────────────────────────────────────────────────────────┘

┌──────────────── 信令旁路 / RTCP 控制面 ────────────────┐
│  [SignalingClient]  ──WebSocket──>  [SignalingServer]   │
│       交换 offer/answer/candidate                        │
│  RTCP: SR/RR（质量统计）+ NACK（重传）+ PLI（要关键帧）   │
└─────────────────────────────────────────────────────────┘
```

注意这张图的三个要点（面试常考）：

1. **媒体数据不经过服务器。** 信令服务器只在"建立连接前"转发 SDP 和 ICE candidate，
   连接一旦建立，音视频直接在对端之间走 P2P。
2. **RTCP 是一条反向的控制流。** 媒体包是"我发给你"，RTCP 里却既有"发送报告 SR"
   又有"接收端反馈 RR/NACK/PLI"，方向是双向的。它跑在和 RTP 同一条通道里（同端口）。
3. **Jitter Buffer 是接收链路的咽喉。** 网络层给它的包是乱序、有丢、有抖动的，
   它吐给解码器的是"尽量有序、尽量连续"的包。

#### 完整目录树（一句话职责）

```
CrystalRTC/
├── CMakeLists.txt               # 根构建脚本：拉取依赖 + 定义 demo/client/server 三个可执行文件
├── main_demo.cpp                # 无硬件依赖的 4 个演示（RTP/FU-A/JitterBuffer/全管道）
├── main_client.cpp              # 完整 P2P 客户端（采集→编码→RTP→传输→解码→渲染 全链路）
├── main_signaling_server.cpp    # C++ 信令服务器入口
├── signaling_server.py          # 另一个 Python 版信令服务器（用于对照）
├── configs/default.json         # 配置文件
├── src/
│   ├── CMakeLists.txt           # 定义各模块的静态库（crystal_media_rtp 等）
│   ├── utils/                   # logger.h/.cpp：spdlog 日志封装
│   ├── room/                    # room_manager.h/.cpp：房间与成员管理
│   ├── signaling/               # signaling_client/server：WebSocket 信令
│   ├── transport/               # transport_manager + ice_config：ICE/DTLS/SRTP
│   └── media/
│       ├── rtp/                 # RTP 打包/解包/抖动缓冲（自研）
│       ├── rtcp/                # RTCP：SR/RR/NACK/PLI + 重传缓冲 + NACK 状态机 + 统计器
│       ├── video/               # H.264 编解码 + V4L2 采集 + SDL 渲染
│       └── audio/               # Opus 编解码 + ALSA 采集 + SDL 播放
├── tests/                       # googletest 单元测试（9 个测试文件）
│   └── CMakeLists.txt
└── docs/                        # 本指南、USAGE.md 及设计文档
```

每个模块一句话记忆：

- **media/rtp** —— 数据怎么装进"信封"、怎么从"信封"取出来、以及收信时怎么排好顺序。
- **media/rtcp** —— "快递投诉系统"：报告丢了多少、让发件方重发、让发件方重发关键帧。
- **media/video** —— 摄像头捕捉（V4L2）、压成 H.264、解回 YUV、画到屏幕（SDL）。
- **media/audio** —— 麦克风捕捉（ALSA）、压成 Opus、解回 PCM、播到扬声器（SDL）。
- **signaling** —— "牵线人"，用 WebSocket 帮两端交换连接信息（libwebsockets）。
- **transport** —— "加密管道"，ICE 打洞 + DTLS 握手 + SRTP 加密（libdatachannel）。
- **room** —— 房间与成员的数据结构（谁在哪个房间）。
- **utils** —— 日志（spdlog）。

#### 线程模型（面试重点）

一个 C++ 音视频程序，最容易被问到的不是协议，而是**"这些回调到底在哪个线程执行"**。
本项目大致有这几类线程：

| 线程 | 谁创建 | 干什么 | 触发的回调 |
|------|--------|--------|-----------|
| 主线程 | `main()` | 主循环：SDL 事件轮询 + RTCP 周期任务（NACK 重试、每 5s 发 SR/RR） | `renderer.pollEvents()`、`nackRequester.tick()` |
| libdatachannel 回调线程 | 库内部 | 收到远端媒体/信令状态变化 | `pc_->onTrack`、`pc_->onRtcp`、`onStateChange` |
| V4L2 采集线程 | `V4L2Capture` | 摄像头 DQBUF 取帧 | `capture.onFrame` → `encoder.encode` → `encoder.onEncoded` |
| ALSA 采集线程 | `AlsaCapture` | 麦克风 `snd_pcm_readi` 取 PCM | `alsaCapture.onAudio` → `opusEncoder.encode` |
| SDL 音频线程 | SDL 内部 | 按需"拉"PCM 播放 | `SDLAudioPlayer::audioCallback`（静态回调） |

**要理解的关键点：**

1. **发送侧的"采集 → 编码 → 打包 → 发送"发生在采集线程里**，是串行的调用链
   （`onFrame` 里同步调用 `encode`，`encode` 里同步触发 `onEncoded`，`onEncoded` 里
   打包并 `sendMedia`）。这一点简化了同步问题。
2. **接收侧发生在 libdatachannel 回调线程里**：`onTrack` 里做 JitterBuffer 插入、
   解包、解码。
3. **RTCP 组件内部都加了 `std::mutex`**，原因正在于"跨线程访问"：
   - `RetransmissionBuffer::store()` 在采集/编码线程调用（发出包后存缓存），
     `get()` 在 RTCP 接收线程调用（对端 NACK 来了查缓存补发）——**两个不同的线程**。
   - `NackRequester::onMissing/onReceived` 在接收线程调用，`tick()` 在主线程调用。
   - `SendSideReporter::onPacketSent` 在采集线程，`onReceiverReport` 在 RTCP 接收线程。
   所以这些类的头文件里都能看到 `mutable std::mutex mutex_;`。如果你面试被问
   "为什么这里要加锁"，答案就是上面这三条。

   具体数据在哪个线程产生，可以这样记：
   - `RetransmissionBuffer` 的内容 → 采集线程 write、RTCP 线程 read；
   - `NackRequester` 的待请求表 → 接收线程写、主线程(`tick`)读；
   - `RecvSideReporter` 的统计 → 接收线程写、主线程(每 5s)读；
   - `SendSideReporter` 的 RTT → 采集线程写计数、RTCP 线程写 RTT、主线程读。
4. **主循环用 `sleep_for(16ms)` 驱动**（约 60fps 轮询），负责一切"按时间触发"的活：
   每 16ms 检查一次有无到期要重试的 NACK，每 5s 发一次 SR/RR 并打印 `[stats]` 行。

#### 面试时如何一句话介绍项目

> "我做了一个 C++ 自研的 WebRTC P2P 音视频通话项目，用 V4L2/ALSA 采集摄像头和麦克风，
> 用 FFmpeg（libx264）和 libopus 分别压缩成 H.264 和 Opus，自己实现了 RTP 打包/解包和
> Jitter Buffer（`std::map` + 双序列号指针），用 libdatachannel 做 ICE/DTLS/SRTP 传输，
> 用 libwebsockets 写了一个信令服务器，并且自己实现了 RTCP 的 SR/RR/NACK/PLI 反馈、重传
> 缓冲和 NACK 重试状态机，从而在丢包时能重传或请求关键帧恢复画面。"

这句话涵盖了采集、编解码、传输、信令、控制面五个层次，面试官一听就知道你懂整体结构。
下面 11 个模块，就是把这句话里的每一个名词展开。

### 动手练习（Module 0）

**练习 0.1：跑通 demo。** 在项目根目录构建并运行无硬件演示，观察四个 Demo 各自输出了什么。

```bash
cmake -B build && cmake --build build
./build/crystal_demo
```

**练习 0.2：对照数据流图读 `main_client.cpp` 的注释头。** 该文件开头有一大段
"模块协作关系 - 完整数据流"的注释（第 30~65 行附近），把它和上文三层架构图互相对照，
找到每条边对应的是哪个回调函数。

### 面试高频题（Module 0）

1. **你的项目里音视频数据经过信令服务器吗？** 不经过。信令服务器只在连接建立阶段转发
   SDP 和 ICE candidate，P2P 连接建立后媒体数据直连传输。这是 WebRTC 的核心特征。
2. **为什么 RTCP 组件内部要加锁？** 因为它们的成员被不同线程读写（发送路径在采集线程、
   接收/重传路径在 libdatachannel 回调线程、周期任务在主线程），用 `std::mutex` 保证
   数据竞争安全。
3. **项目用的传输库和信令库分别是什么？** 传输层用 libdatachannel（封装 ICE/DTLS/SRTP），
   信令用 libwebsockets（WebSocket 客户端/服务器）。
4. **你的主循环在干什么？** 用 `sleep_for(16ms)` 轮询：处理 SDL 窗口事件、驱动 NACK 重试
   (`tick`)、每 5s 发 SR/RR 并打印 `[stats]` 统计行、检测"重传放弃"后发 PLI。

---

## Module 1：RTP 协议

### 概念讲解

RTP（Real-time Transport Protocol，实时传输协议，RFC 3550）是 WebRTC 传输音视频数据的
"通用信封"。不管是 H.264 视频还是 Opus 音频，都要先装进 RTP 包，再交给 UDP 发出去。

#### 为什么是 UDP + RTP，而不是 TCP？

这是面试第一问。核心原因是**实时性优先于可靠性**：

- TCP 会重传丢失的包，这带来**递增的延迟**。视频通话里，一个"迟到了 500ms 的包"比
  "永远丢掉的包"更糟糕——迟到意味着画面卡顿和积压，丢失最多是这一帧花一下。
- TCP 还有队头阻塞（head-of-line blocking）：一个包没确认，后面的包都得排队。
- UDP 不保证到达、不保证顺序，但**快**。于是我们在 UDP 之上用 RTP 补上"序号"和
  "时间戳"这两样最必要的信息，让接收端有能力自己处理乱序和丢包。

一句话总结：**TCP 用"重传"换可靠，代价是延迟；RTP/UDP 用"序号+反馈"在必要时有选择地重传
（NACK），代价是偶尔丢包。实时通话选后者。**

#### RTP 12 字节固定头：每个字段"为什么存在"

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|V=2|P|X|  CC   |M|     PT      |       sequence number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           synchronization source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

逐字段"为什么存在"（本项目对应的取值）：

| 字段 | 位数 | 本项目取值 | 为什么存在 |
|------|------|-----------|-----------|
| V（版本） | 2 | 2 | 协议版本号，固定 2，接收端用它做最粗的合法性校验 |
| P（填充） | 1 | 0 | 某些加密算法要求对齐，本项目不用 |
| X（扩展） | 1 | 0 | 头部扩展位，本项目不用 |
| CC（CSRC 计数） | 4 | 0 | 混流器场景用于标记多个贡献源，本项目不做混流 |
| M（标记位） | 1 | 视频=帧尾包 / 音频=1 | **标记帧边界**：视频里标记"一帧的最后一个分片" |
| PT（负载类型） | 7 | 96=H.264 / 97=Opus | 区分这条流装的是什么编码 |
| Sequence Number | 16 | 递增，随机起点 | **防乱序、检测丢包**：靠序号间隙判断丢了几个包 |
| Timestamp | 32 | 视频 90000Hz / 音频 48000Hz | **采样时刻**：音画同步、抖动计算、判断哪些包属于同一帧 |
| SSRC | 32 | `std::random_device` 随机 | **区分流**：同一连接里音频流和视频流用不同 SSRC 标识 |

需要特别讲清 `M` 位和 `Timestamp` 的关系（易混淆）：

- **一个视频帧可能被拆成多个 RTP 包**（大帧要做 FU-A 分片，见 Module 2）。这多个包
  有**相同的 Timestamp**（它们是同一帧），但**递增的 Sequence Number**（它们是不同包）。
  最后一个包的 `M=1` 表示"这一帧到此结束"。
- **SeqNum 每个包 +1**，用于排序和丢包检测；**Timestamp 每帧 +90kHz/fps**，用于同步。
  两者单位不同、步进不同，千万别混。

#### 为什么视频时钟是 90000Hz

因为 90000 能够被所有常见帧率整除，保证"每帧推进的时间戳增量是整数"：

| 帧率 | 每帧 tick 增量 | 是否整数 |
|------|---------------|---------|
| 30 fps | 90000 / 30 = 3000 | ✅ |
| 24 fps | 90000 / 24 = 3750 | ✅ |
| 25 fps | 90000 / 25 = 3600 | ✅ |
| 60 fps | 90000 / 60 = 1500 | ✅ |

而音频时钟统一用采样率 48000Hz（因为 Opus 内部工作在 48kHz），20ms 一帧即
`48000 × 0.02 = 960` 个 tick。在 `main_client.cpp` 里你能看到这两句：

```cpp
videoRtpTs += 90000 / encConfig.fps;   // 视频：每帧推进 90000/fps
audioRtpTs += static_cast<uint32_t>(opusEncoder.frameSize()); // 音频：每帧推进 960
```

#### 大端序（Big-Endian）是什么

RTP 头里所有多字节字段（SeqNum、Timestamp、SSRC）都按**网络字节序 = 大端序**传输，即
"高位字节在前"。你的 CPU（x86/ARM）通常是小端序（低位字节在前），所以解析和序列化时要
手动把字节拼起来/拆开来。类比：大端序就像我们写数字"12"，先写十位 1 再写个位 2（高位在前）；
小端序则像先写个位 2 再写十位 1。RTP 统一规定用大端序，就是为了不同架构的机器互通。

### 代码精读

文件：`src/media/rtp/rtp_packet.h` / `rtp_packet.cpp`。核心是两个函数：`parse()` 和 `serialize()`。

#### parse()：从字节流解析（逐段讲字节拆分）

```cpp
bool RtpPacket::parse(const uint8_t* data, size_t size) {
    // 第一步：RTP 固定头部至少 12 字节
    if (size < 12) {
        Logger::error("RTP packet too short: {} bytes", size);
        return false;
    }

    uint8_t byte0 = data[0];
    uint8_t byte1 = data[1];

    // 第三步：验证版本号，byte0 >> 6 提取最高 2 位
    if ((byte0 >> 6) != 2) {
        Logger::error("Invalid RTP version: {}", byte0 >> 6);
        return false;
    }

    // 第四步：提取 CSRC 计数，计算实际头部长度 = 12 + CC*4
    uint8_t cc = byte0 & 0x0F;
    size_t header_len = 12 + cc * 4;
    if (size < header_len) {
        Logger::error("RTP packet too short for CSRC: {} < {}", size, header_len);
        return false;
    }

    // 第五步：提取 M 和 PT
    marker_ = (byte1 & 0x80) != 0;   // 最高位是 M
    payloadType_ = byte1 & 0x7F;     // 低 7 位是 PT

    // 第六步：序列号（16 位大端序）
    sequenceNumber_ = (static_cast<uint16_t>(data[2]) << 8) | data[3];

    // 第七步：时间戳（32 位大端序）
    timestamp_ = (static_cast<uint32_t>(data[4]) << 24) |
                 (static_cast<uint32_t>(data[5]) << 16) |
                 (static_cast<uint32_t>(data[6]) << 8) |
                 data[7];

    // 第八步：SSRC（32 位大端序）
    ssrc_ = (static_cast<uint32_t>(data[8]) << 24) |
            (static_cast<uint32_t>(data[9]) << 16) |
            (static_cast<uint32_t>(data[10]) << 8) |
            data[11];

    // 第九步：负载从头部之后开始
    size_t payload_len = size - header_len;
    if (payload_len > 0) {
        payload_.assign(data + header_len, data + header_len + payload_len);
    } else {
        payload_.clear();
    }
    return true;
}
```

逐行要点：

- `byte0 >> 6` 是**无符号移位**，把高 2 位版本号挪到最低位。`0x80` 右移 6 位是 `0x02`，
  正好等于 2。
- `byte1 & 0x80` 提取 M 位（`0x80 = 1000 0000`）；`byte1 & 0x7F` 提取 PT（`0x7F = 0111 1111`）。
- 大端序组装 16 位：`(高字节 << 8) | 低字节`；32 位依次 `<< 24 | << 16 | << 8 | 低字节`。
  脑子里记住一个口诀：**"左移高位、右合并"**。

#### serialize()：把结构体拼回字节流

```cpp
std::vector<uint8_t> RtpPacket::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(12 + payload_.size());

    uint8_t byte0 = (2 << 6) | 0x00;   // V=2 占最高 2 位，P/X/CC 全 0
    buf.push_back(byte0);

    uint8_t byte1 = (marker_ ? 0x80 : 0x00) | (payloadType_ & 0x7F);
    buf.push_back(byte1);

    buf.push_back(static_cast<uint8_t>(sequenceNumber_ >> 8));          // 序列号高字节
    buf.push_back(static_cast<uint8_t>(sequenceNumber_ & 0xFF));        // 序列号低字节

    buf.push_back(static_cast<uint8_t>(timestamp_ >> 24));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((timestamp_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(timestamp_ & 0xFF));

    buf.push_back(static_cast<uint8_t>(ssrc_ >> 24));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((ssrc_ >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(ssrc_ & 0xFF));

    buf.insert(buf.end(), payload_.begin(), payload_.end());
    return buf;
}
```

这正好是 `parse()` 的逆过程：主机序的多字节整数按大端序"高位字节先 push"。

### 动手练习

**练习 1.1（观察字节流）：** 跑 `./crystal_demo`，看 Demo 1 输出的字节流，手算验证：

- Demo 里设了 `marker=true, PT=96, seq=42, ts=90000, ssrc=0xDEADBEEF`，负载
  `{0x65, 0x88, 0x84, 0x00, 0x40}`。序列化后前 12 字节应该是：
  - 第 0 字节 `80` = `1000 0000`（V=2，P/X/CC=0）
  - 第 1 字节 `E0` = `1110 0000`（M=1，PT=96=0x60）
  - 第 2-3 字节 `00 2A`（42 = 0x002A）
  - 第 4-7 字节 `00 01 5F 90`（90000 = 0x00015F90）
  - 第 8-11 字节 `DE AD BE EF`

**练习 1.2（手算一个 RTP 头）：** 不看代码，构造一个 `marker=false, PT=97, seq=65535,
  ts=960, ssrc=1` 的包，手写它的 12 字节头，再用程序验证。

答案提示：第 1 字节 = `0x00 | 97` = `0x61`；seq `65535` → `FF FF`；ts `960` →
`00 00 03 C0`；ssrc `1` → `00 00 00 01`。

**练习 1.3（防御性检查）：** 在 `rtp_packet.cpp` 的 `parse()` 里，如果版本号不是 2 会
返回什么？为什么这个检查重要？（答：返回 `false` 丢弃该包——防止把非 RTP 数据当媒体处理，
也防止被攻击者伪造包干扰。）

### 面试高频题

1. **RTP 和 UDP 是什么关系？** RTP 运行在 UDP 之上。UDP 只负责把字节从 A 送到 B（不保证
   顺序、不保证到达），RTP 在 UDP 的负载里定义了序号、时间戳、SSRC 等字段，让接收端能
   自己处理乱序、丢包和分流。
2. **为什么视频时钟是 90000Hz？** 因为 90000 能被常见帧率（24/25/30/60）整除，使"每帧
   时间戳增量"是整数，避免累计取整误差。
3. **Sequence Number 和 Timestamp 的区别？** SeqNum 每包 +1，用于排序和丢包检测；
   Timestamp 每帧推进（视频 90000/fps、音频 960），用于音画同步和抖动计算。一个视频帧
   若被 FU-A 拆成多个包，它们 Timestamp 相同、SeqNum 递增。
4. **M 位有什么作用？** 标记帧边界。视频中 M=1 表示一帧的最后一个 RTP 包（接收端据此
   知道"这帧收齐了，可以送解码器"）。
5. **RTP 与 RTCP 是什么关系？** RTCP 是 RTP 的控制协议，跑在相邻端口或（本项目的
   RFC 5761 方式）同一端口。RTP 传媒体，RTCP 反馈网络质量并支撑重传/关键帧请求。
   （详见 Module 8。）

---

## Module 2：H.264 FU-A 打包

### 概念讲解

一个 H.264 IDR 关键帧可能有几十 KB，而网络链路的 MTU（最大传输单元，通常 1500 字节）
装不下。把一个超过 MTU 的 NAL 单元拆成多个 RTP 包传输，拆和装的标准做法就是 **FU-A
（Fragmentation Unit type A）**，RFC 6184 定义。

#### 三种打包模式：决策树

| 模式 | 触发条件 | 说明 |
|------|---------|------|
| 单 NAL 单元（Single NAL） | NAL ≤ maxPacketSize | 一个 NAL 直接塞进一个 RTP 负载 |
| FU-A 分片 | NAL > maxPacketSize | 一个大 NAL 拆成多个 RTP 包 |
| STAP-A 聚合 | 多个小 NAL 且想省包 | 多个小 NAL 打包进一个 RTP 包（本项目仅打了日志，未完整实现） |

决策逻辑在 `RtpPacketizer::packetizeH264()` 里就一句话：

```cpp
if (nalUnit.size() <= maxPacketSize_) {
    // 单 NAL 模式
} else {
    // FU-A 分片模式
}
```

其中 `maxPacketSize_` 默认 1200 字节（`rtp_packetizer.h` 构造参数），这个值是考虑了
MTU 1500 − IP 头 20 − UDP 头 8 = 1472 后，再留安全余量取的 1200，保证穿越各种网络设备。

#### FU-A 的两个额外字节

FU-A 分片时，原来的 NAL 头（1 字节）被拆成了两个字节：**FU Indicator** 和 **FU Header**。

```
原始 NAL Header:   [F(1)] [NRI(2)] [Type(5)]      ← 1 字节
FU-A 负载:         [FU Indicator] [FU Header] [分片数据...]
                   [F(1)][NRI(2)][Type=28(5)]      ← FU Indicator
                   [S(1)][E(1)][R(1)][Type(5)]     ← FU Header
```

**FU Indicator（第 1 字节）：**

- `forbidden_zero_bit`（1 bit）= 0（始终）
- `nal_ref_idc`（2 bit）= 原始 NAL 的重要性等级（原样保留）
- `type`（5 bit）= 28（表示这是 FU-A）

**FU Header（第 2 字节）：**

- `S`（Start）= 1 表示这是第一个分片
- `E`（End）= 1 表示这是最后一个分片
- `R`（Reserved）= 0
- `type`（5 bit）= 原始 NAL 的类型（如 5=IDR、1=非 IDR 切片）

只有 Start 包的 S=1，只有 End 包的 E=1，中间的包 S=E=0。**只有 End 包的 RTP 头 M 位 = 1**。

#### 为什么 FU-A 要保留原始 NAL 的 nal_ref_idc（面试必考）

`nal_ref_idc`（NRI）表示这个 NAL 的重要性：3=最重要（IDR/SPS/PPS），2=参考帧，0=可丢弃。
FU-A 把它从原始 NAL 头"抄"到 FU Indicator 里，是为了让**中途的网元（如丢包策略、拥塞控制）
在只看到分片头时就知道这个包重不重要**，从而决定丢不丢、优先转发谁。如果 Receiver 拿不到
NRI，就无从判断"这个分片丢了会不会导致后续整串帧解不出来"。

### 代码精读

文件：`src/media/rtp/rtp_packetizer.cpp` 的 `packetizeH264()`。

#### 分片：提取 NRI、切数据、构造两字节头

```cpp
std::vector<RtpPacket> RtpPacketizer::packetizeH264(
    const std::vector<uint8_t>& nalUnit, uint32_t timestamp) {

    std::vector<RtpPacket> result;

    if (nalUnit.size() <= maxPacketSize_) {
        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);
        pkt.setSsrc(ssrc_);
        pkt.setMarker(true);          // 单 NAL 模式：这包就是帧尾，M=1
        pkt.setPayload(nalUnit);
        result.push_back(std::move(pkt));
        return result;
    }

    // ---- FU-A 分片模式 ----
    uint8_t nalRefIdc = (nalUnit[0] & 0x60) >> 5;  // 提取 NRI（bit5-4）
    uint8_t nalType = nalUnit[0] & 0x1F;           // 提取 NAL 类型（bit4-0）

    const uint8_t* nalData = nalUnit.data() + 1;   // 跳过原始 NAL 头
    size_t nalDataLen = nalUnit.size() - 1;

    size_t maxFragLen = maxPacketSize_ - 2;        // 每个分片减 2 字节头开销

    size_t offset = 0;
    while (offset < nalDataLen) {
        size_t fragLen = std::min(maxFragLen, nalDataLen - offset);
        bool isFirst = (offset == 0);
        bool isLast = (offset + fragLen >= nalDataLen);

        std::vector<uint8_t> fuPayload;
        fuPayload.reserve(2 + fragLen);

        uint8_t fuIndicator = (nalRefIdc << 5) | 28; // NRI 回到 bit6-5，type=28
        fuPayload.push_back(fuIndicator);

        uint8_t fuHeader = (nalType & 0x1F);
        if (isFirst) fuHeader |= 0x80;               // Start bit
        if (isLast)  fuHeader |= 0x40;               // End bit
        fuPayload.push_back(fuHeader);

        fuPayload.insert(fuPayload.end(), nalData + offset,
                         nalData + offset + fragLen);

        RtpPacket pkt;
        pkt.setPayloadType(payloadType_);
        pkt.setSequenceNumber(sequenceNumber_++);
        pkt.setTimestamp(timestamp);   // 同一帧的所有分片共享同一时间戳
        pkt.setSsrc(ssrc_);
        pkt.setMarker(isLast);         // 只有最后一个分片 M=1
        pkt.setPayload(fuPayload);
        result.push_back(std::move(pkt));

        offset += fragLen;
    }
    return result;
}
```

关键点：

- `nalUnit[0] & 0x60 >> 5` 提取 NRI：`0x60 = 0110 0000` 是 bit6-5 的掩码，右移 5 位后
  变成 `0~3` 的一个数。
- `maxFragLen = maxPacketSize_ - 2`：因为每个分片都要额外带 2 字节（FU Indicator +
  FU Header），所以真正的数据每片最多只有 `maxPacketSize_ - 2`。
- 首片 `S=1`、尾片 `E=1`、尾片 `M=1`；中片三样都不置位。

#### 重组：depacketizeH264 的状态机

文件：`src/media/rtp/rtp_depacketizer.cpp`。接收端要处理三种情形：单 NAL、FU-A、STAP-A。

```cpp
std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeH264(
    const RtpPacket& pkt) {
    std::vector<std::vector<uint8_t>> result;
    const auto& payload = pkt.payload();
    if (payload.empty()) return result;

    uint8_t nalType = payload[0] & 0x1F;   // 看负载第 1 字节的低 5 位

    if (nalType >= 1 && nalType <= 23) {
        // 单 NAL：如果此前正在重组 FU-A，说明 FU-A 不完整，丢弃
        if (fuStarted_) {
            fuStarted_ = false;
            fuBuffer_.clear();
        }
        result.push_back(payload);
    }
    else if (nalType == 28) {
        if (payload.size() < 2) return result;
        uint8_t fuHeader = payload[1];
        bool startBit = (fuHeader & 0x80) != 0;
        bool endBit   = (fuHeader & 0x40) != 0;
        uint8_t originalNalType = fuHeader & 0x1F;

        if (startBit) {
            fuStarted_ = true;
            fuBuffer_.clear();
            // 重建 NAL 头：NRI 来自 FU Indicator，type 来自 FU Header
            uint8_t nalRefIdc = (payload[0] & 0x60);
            uint8_t reconstructedNal = nalRefIdc | originalNalType;
            fuBuffer_.push_back(reconstructedNal);
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2, payload.end());
            }
        }
        else if (fuStarted_) {
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2, payload.end());
            }
            if (endBit) {
                fuStarted_ = false;
                result.push_back(std::move(fuBuffer_));
                fuBuffer_.clear();
            }
        }
        else {
            Logger::warn("FU-A middle/end without start, dropping");
        }
    }
    else if (nalType == 24) {
        Logger::debug("STAP-A packet received (not yet fully supported)");
    }
    return result;
}
```

重组状态机读法（`fuStarted_` 是"是否正在收集 FU-A"的布尔标志）：

```
[空闲](fuStarted_=false)
   │ 收到 Start 包 → 重建 NAL 头，进入 [收集中](fuStarted_=true)
   │ 收到中间包 → 追加数据，仍在 [收集中]
   │ 收到 End 包 → 追加数据，输出完整 NAL，回到 [空闲]
   │ 收到新的 Start 包（上一个没结束）→ 丢弃旧缓冲，重新开始
   │ 收到中间/End 包但没有 Start → 丢弃（可能 Start 包丢了）
```

**畸形 FU-A 序列的丢弃策略**在代码里体现为三处：单 NAL 到达时清空 `fuBuffer_`；新 Start
到达时覆盖旧缓冲；无 Start 的中间/End 包直接丢弃。这是容错的关键——宁可丢一帧，也不要
把错的数据喂给解码器。

### 动手练习

**练习 2.1（观察 Demo 2）：** 跑 `./crystal_demo`，观察一个大于 `maxPacketSize_` 的 NAL
被拆成几个包，每个包的 `S/E` 位和 `M` 位分别是什么。

**练习 2.2（手算分片数）：** 若 `maxPacketSize=1200`、NL 大小为 3000 字节（含 1 字节 NAL 头），
需要几个 FU-A 包？（答：数据体 2999 字节，每片最多 1198 字节，`ceil(2999/1198)=3` 片。)

**练习 2.3（进阶-改 MTU）：** 把 `rtp_packetizer.cpp` 的 `maxPacketSize_` 从 1200 改到 500
（或改 `main_demo` 里传入的构造参数），重新编译运行，观察分片数变多、每个包更小。
思考：`maxPacketSize` 变小会带来什么副作用？（答：同样的 NAL 拆更多包，每个都有 RTP 头
开销，带宽利用率下降。）

### 面试高频题

1. **FU-A 的 start 包和 end 包的 marker 位分别是什么？** 只有 end 包（也可能是单 NAL 包）
   的 M=1，start 和 middle 包 M=0。M 位标记的是"帧结束"而不是"分片开始"。
2. **接收端如何知道一个 FU-A 序列结束了？** 收到 FU Header 的 `E=1` 的包，就完成重组输出。
3. **为什么 FU-A 要保留原始 NAL 的 nal_ref_idc？** 让网络中间节点和接收端在只看分片头时
   就知道该包重要性（丢它会不会连累后续帧），便于丢包决策和优先级处理。
4. **如果 FU-A 的中间某个包丢了，接收端怎么处理？** 短答：丢弃整个 FU-A 序列，等待下一个
   IDR 关键帧。更完整的答案：解包器检测到 FU-A 不完整（下次单 NAL 或新 Start 到达时清空
   缓冲），不会把缺片的数据送给解码器；同时丢包会触发 NACK 重传或 PLI 请求关键帧
   （Module 8）。
5. **STAP-A 和 FU-A 的区别？** FU-A 是把一个 NAL 拆成多包（解决"太大"）；STAP-A 是把多个
   小 NAL 合成一包（解决"太小、头开销浪费"）。

---

## Module 3：Jitter Buffer

### 概念讲解

UDP 不保证顺序、不保证到达、间隔也不均匀。接收端拿到的 RTP 包有三种"异常"：

1. **乱序**：后发的包先到了（`seq 102` 比 `seq 101` 先到）。
2. **抖动**：包与包之间的到达间隔忽快忽慢（网络排队时间在变化）。
3. **丢包**：`seq 101` 永远没来。

Jitter Buffer（抖动缓冲）就干三件事：**缓存**（等一等迟到的包）、**排序**（按 seq 重排）、
**丢包检测**（发现 seq 空洞）。

#### 为什么需要"缓冲"本身

如果收到一个包就立刻给解码器，那乱序的时候顺序就错了、抖动的时候就会"卡一下走一下"。
所以接收端故意**延迟一小段时间**（比如 40ms）再输出，给那些"迟到但没丢"的包一个机会。
缓冲越大 → 抗抖动越强但延迟越高；缓冲越小 → 延迟低但更容易因为等不到包而把"迟到"误判成"丢失"。

#### 双指针设计：为什么必须两个指针（重点）

本项目的 `JitterBuffer` 用了两个独立的序列号指针，这是**修复过一个 bug 之后的设计**，
面试里属于"能讲出这个，说明你真的读过代码"的加分点：

- `expectedSeq_`：由 `insert()` 独占维护，语义是**"下一个期望从网络收到的 seq"**，
  用于**丢包检测**。
- `nextOutputSeq_`：由 `consume()` 独占维护，语义是**"下一个期望从缓冲区输出的 seq"**，
  用于**排序输出**。

**为什么不能共用一个指针？** 因为 `consume()` 在输出"迟到包"时要"回退"序列号指针，
如果这个指针同时被 `insert()` 用来判断丢包，就会重复把"已收到的包"算成丢包。

看一个共用变量的错误场景（`jitter_buffer.h` 注释里就有这份说明）：

```
共用变量的错误：
  insert(100): 指针=101
  insert(105): 指针=106, 报 4 个丢包
  consume(): 输出 100，指针回退到 101
  insert(106): diff = 106-101 = 5，又多算 5 个丢包！← 错误（106 明明已收到）

分离后：
  insert(100): expectedSeq_=101
  insert(105): expectedSeq_=106, 报 4 个丢包
  consume(): 输出 100, nextOutputSeq_=101（不影响 expectedSeq_）
  insert(106): diff = 106-106 = 0, 无丢包 ← 正确
```

一句话记：**`insert()` 管"从网络看丢没丢"，`consume()` 管"从输出看该不该吐"；两个指针
分工不同，互不写入对方。**

#### int16_t 回绕技巧：完整示例表

Sequence Number 是 16 位无符号（0~65535），65535 之后回绕到 0。判断"seq 比 expectedSeq
大还是小"时，如果直接比较 `uint16_t`，回绕点附近会判断错。技巧是把无符号差值转成有符号：

```cpp
int16_t diff = static_cast<int16_t>(seq - expectedSeq_);
```

- `diff` 在 `0~32767` → `seq >= expectedSeq`（正常递增或相等）；
- `diff` 在 `-32768~-1` → `seq < expectedSeq`（乱序/重复）。

完整示例表（`expectedSeq` 语义是"下一个期望收到的 seq"）：

| expectedSeq | seq | diff | 含义 |
|-------------|-----|------|------|
| 101 | 101 | 0 | 正常，收到期望的包 |
| 101 | 102 | 1 | 跳了 1 个包，seq 101 丢了 |
| 101 | 103 | 2 | 跳了 2 个包，seq 101、102 丢失 |
| 65535 | 0 | 1 | 回绕，仍判断为正常递增（跳 1 包） |
| 101 | 100 | -1 | 乱序/迟到的包 |
| 1 | 65534 | -3 | 回绕方向的乱序 |

**注意：丢包数的正确表述是"丢包数 = diff"，不是"diff - 1"。** 因为 `expectedSeq_` 的语义
已经是"下一个期望收到的 seq"（即"已收到的下一个"），所以 `seq - expectedSeq_` 就等于
真正的空洞个数。例如 `expectedSeq=101` 收到 `seq=103`，`diff=2`，丢的正是 101 和 102 两个包。

### 代码精读

文件：`src/media/rtp/jitter_buffer.h` / `jitter_buffer.cpp`。

#### insert()：插入 + 丢包检测 + 上报 missing

```cpp
std::vector<uint16_t> JitterBuffer::insert(const RtpPacket& pkt) {
    std::vector<uint16_t> missing;   // 本次新检测到的丢失 seq（给上层发 NACK）
    receivedCount_++;

    if (firstPacket_) {
        expectedSeq_ = pkt.sequenceNumber() + 1;
        nextOutputSeq_ = pkt.sequenceNumber();
        firstPacket_ = false;
        buffer_[pkt.sequenceNumber()] = pkt;
        return missing;
    }

    uint16_t seq = pkt.sequenceNumber();
    int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

    if (diff == 0) {
        buffer_[seq] = pkt;
        expectedSeq_ = seq + 1;
    } else if (diff > 0) {
        lostCount_ += static_cast<uint64_t>(diff);   // 丢包数 = diff
        if (diff <= kMaxNackGap) {                   // kMaxNackGap = 64
            for (int i = 0; i < diff; i++) {
                missing.push_back(static_cast<uint16_t>(expectedSeq_ + i));
            }
        } else {
            Logger::warn("JitterBuffer: huge gap ({}), skip NACK report", diff);
        }
        buffer_[seq] = pkt;
        expectedSeq_ = seq + 1;
    } else {
        // diff < 0：迟到的/重复的包，只插入，不重复计数
        buffer_[seq] = pkt;
        Logger::debug("JitterBuffer: late/duplicate packet seq={}", seq);
    }
    return missing;
}
```

三个关键点（务必记牢）：

1. `lostCount_ += diff` —— 丢包数就是 `diff`。
2. **`insert()` 的返回值是 `std::vector<uint16_t>`**，即"本次新检测到的丢失 seq 列表"，
   供上层触发 NACK。**只有间隙 ≤ `kMaxNackGap`(64) 时才逐个上报**；间隙 > 64 只记
   `warn` 不上报。
3. 为什么要设 64 这个阈值？正常网络抖动不会一次连丢几十个包，大间隙通常意味着码流中断
   /重连，此时点对点 NACK 重传几十个包既不划算还会造成 **NACK 风暴**。大间隙的画面恢复
   交给 **PLI（请求关键帧）**，这就是 `insert()` 与 Module 8 NACK/PLI 联动的接口。

#### consume()：有序输出（pull 驱动）

```cpp
std::vector<RtpPacket> JitterBuffer::consume() {
    std::vector<RtpPacket> result;
    if (buffer_.empty()) return result;

    auto it = buffer_.begin();   // std::map 已按 seq 排序
    while (it != buffer_.end()) {
        uint16_t seq = it->first;
        int16_t diff = static_cast<int16_t>(seq - nextOutputSeq_);

        if (diff <= 0) {
            result.push_back(it->second);   // 已到期/迟到，输出
            nextOutputSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else if (diff <= 3) {
            nextOutputSeq_ = seq;           // 小间隙（1-3），判为丢包，跳过等待
            result.push_back(it->second);
            nextOutputSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else {
            ++it;                            // 大间隙，可能还在路上，等待
        }
    }
    return result;
}
```

输出策略总结：

| 条件 | 策略 | 含义 |
|------|------|------|
| `diff <= 0` | 立即输出 | 包已到期（或迟到），该吐了 |
| `1 <= diff <= 3` | 跳过缺失立即输出 | 小间隙，判定为丢包，不再等 |
| `diff > 3` | 跳过等待 | 可能还在路上，等下一轮 insert 再看 |

关于 `consume()` 的准确理解：它是**由上层 pull 驱动**的（每次收到新包或轮询时调用一次）。
构造函数虽然有 `targetDelayMs`（默认 40ms），但当前实现**只是存储了这个值，并没有用它做
"基于时间的延迟释放"**——所以它还不是真正按时间排程的生产级 Jitter Buffer，这可以作为
后面的进阶练习。

#### 为什么用 std::map

`buffer_` 是 `std::map<uint16_t, RtpPacket>`。理由（`jitter_buffer.h` 注释里写了）：

1. 自动按 seq 排序，便于按序输出；
2. 支持 O(log n) 查找、删除，能处理 seq 不连续（丢包产生空洞）的情况；
3. 时间复杂度对几十个包的缓冲规模完全够用。

（顺带一提：音频不做 NACK，因为 Opus 自带 PLC 丢包隐藏，重传到达时早已错过播放时刻——
详见 Module 5 和 Module 8。）

### 动手练习

**练习 3.1（观察 Demo 3）：** 跑 `./crystal_demo` 的 Demo 3，观察乱序、丢包、重复包三种
场景分别被 Jitter Buffer 如何处理。

**练习 3.2（模拟回绕）：** 在 `main_demo.cpp` 里先插 `65534, 65535` 再插 `0, 1`，
验证 Jitter Buffer 能否用 `int16_t` 技巧正确处理回绕而不误判。

**练习 3.3（进阶-实现基于时间的延迟释放）：** 当前 `consume()` 是"立即输出到期/小间隙包"，
并没有真正利用 `targetDelayMs_` 做延迟。请实现一个基于时间的版本：每个包记录到达时间，
只有"已缓冲 ≥ targetDelayMs"的包才允许输出。提示：可在 `buffer_` 里存 `(pkt, arrivalMs)`
的映射，或用 `std::chrono::steady_clock` 记录插入时刻。

### 面试高频题

1. **Jitter Buffer 太大和太小分别有什么问题？** 太大 → 端到端延迟高（音画延迟、互动差）；
   太小 → 迟到的包来不及等就被判丢，花屏/卡顿变多。
2. **如何检测 Sequence Number 回绕？** 用 `int16_t diff = (int16_t)(seq - expected)`；当
   diff 为正且在合理范围（不超阈值）时视为正常递增，即使发生了 65535→0 的回绕。
3. **丢包数怎么算？举例说明。** 丢包数 = 当前 seq 与期望 seq 的差值 `diff`（`expectedSeq`
   是"下一个期望收到的 seq"）。例：期望 101，收到 103，`diff=2`，丢 101、102 两个包。
4. **为什么大间隙（>64）不报 NACK？** 防止 NACK 风暴：连丢几十个包说明码流可能中断，
   逐包重传不划算且挤占带宽，改为交给 PLI 请求关键帧整体恢复。
5. **为什么这里要有两个序列号指针？** 因为丢包检测（insert）和排序输出（consume）对
   "进度"的语义不同，consume 回退指针会干扰 insert 的丢包判断，必须分离。

---

## Module 4：H.264 编解码

### 概念讲解

视频原始数据（YUV420P）非常庞大，640×480@30fps 一帧约 460KB、一秒约 13.8MB，直接传
网络根本扛不住。H.264（AVC）是 WebRTC 早期强制支持的视频压缩标准，本模块用 FFmpeg 的
libx264 做编码和解码。

#### NAL 单元类型表（面试必记）

NAL（Network Abstraction Layer）单元是 H.264 码流的最小传输单位，第 1 字节的低 5 位是类型：

| Type | 名称 | 说明 |
|------|------|------|
| 1 | 非 IDR 切片 | P 帧（或 B 帧）数据，依赖参考帧 |
| 5 | IDR 切片 | 关键帧，可独立解码 |
| 6 | SEI | 补充增强信息 |
| 7 | SPS | 序列参数集（分辨率、帧率、Profile 等） |
| 8 | PPS | 图像参数集（熵编码模式、参考帧数等） |

NAL 首字节结构：

```
forbidden_zero_bit (1bit) = 0        // 必须为 0
nal_ref_idc         (2bit) = 参考等级 // 3=IDR/SPS/PPS，2=P 帧，0=可丢弃
type                (5bit) = 见上表
```

**关键顺序：解码器必须先收到 SPS + PPS，才能解码 IDR。** 没 SPS/PPS 时，即使来了 IDR，
解码器也不知道分辨率和参数，无法工作。所以首帧到来时序一定是 `SPS → PPS → IDR → P...`。

#### 为什么实时通话的这些参数要这样配

`h264_encoder.cpp` 的 `init()` 里有一组"拧死"的参数，每一个都有"为什么"：

| 参数 | 取值 | 为什么 |
|------|------|--------|
| `max_b_frames` | 0 | **禁用 B 帧**：B 帧要参考"未来帧"才能解码，引入重排延迟，实时通话等不起 |
| `preset` | `ultrafast` | 牺牲压缩率换最快编码速度，降低编码耗时对延迟的贡献 |
| `tune` | `zerolatency` | 关闭 lookahead/cutree 等会缓冲未来帧、引入延迟的特性 |
| `profile` | `baseline` | 基线 Profile：无 B 帧、无 CABAC、兼容性最好、延迟最低 |
| `gop_size` | 30 | 每 30 帧一个 IDR，平衡压缩率（IDR 大）与随机访问/抗丢包能力 |
| `forced-idr` | 1 | 让"请求关键帧"时编出的真是 IDR（可立即刷新对端画面）而非普通 I 帧 |

**为什么 forced-idr 这个细节很关键？** 普通 I 帧和 IDR 帧的区别：IDR（Instantaneous
Decoder Refresh）会**清空参考帧缓冲区**，后续帧只能从它开始预测；普通 I 帧仍可引用旧
参考帧。当对端画面已经花屏（参考帧链断了），你发给它一个普通 I 帧它照样解不出来，必须发
**IDR** 才能"一刀切"重置画面。这正是 PLI（请求关键帧）能恢复画面的原因。

#### FFmpeg 的 send/receive 双队列异步模型

编码和解码都用"发送-接收"两个队列：

```
编码：avcodec_send_frame(帧)  → [编码器输入队列] → 内部编码 → [输出队列] → avcodec_receive_packet(包)
解码：avcodec_send_packet(包) → [解码器输入队列] → 内部解码 → [输出队列] → avcodec_receive_frame(帧)
```

为什么不直接"喂一帧、拿一帧"？因为**一次 send 不保证立即有输出**：

- 编码侧：一个 IDR 帧可能同时吐出 SPS、PPS、IDR 切片好几个 packet（`encode()` 里用
  `while (avcodec_receive_packet(...) == 0)` 来把输出队列**全部取空**）；
- 解码侧：一个 packet（如 SPS）可能不产生任何帧，需要多个 packet 才凑出一帧（`decode()`
  里同样用 `while (avcodec_receive_frame(...) == 0)` 取空）。

所以 `receive_*` 返回 `AVERROR(EAGAIN)` 表示"暂无输出"，这是正常现象，不是错误。

#### forceKeyframe 与 PLI 的联动，以及一个坑

当收到对端的 PLI（请求关键帧）时，`H264Encoder::forceKeyframe()` 只是置一个标志：

```cpp
void H264Encoder::forceKeyframe() {
    forceKeyframe_ = true;
}
```

真正的动作在 `encode()` 里执行（编码由采集帧驱动，调用方无法直接触发一次编码）：

```cpp
if (forceKeyframe_) {
    frame_->pict_type = AV_PICTURE_TYPE_I;   // 显式指定 I 帧
    forceKeyframe_ = false;                  // 消费后必须复位
} else {
    frame_->pict_type = AV_PICTURE_TYPE_NONE; // 交回编码器按 GOP 决定
}
```

两个注意点：

1. **`frame_` 是复用对象，`pict_type` 必须每帧复位**：如果不复位，`AV_PICTURE_TYPE_I`
   会被消费一次后永久保持，导致每一帧都是关键帧，码率暴涨。
2. **用的是 `AV_PICTURE_TYPE_I` 而不是某些资料里的 `AV_PICTURE_TYPE_IDR`**：
   FFmpeg 里其实**不存在** `AV_PICTURE_TYPE_IDR` 这个枚举（这是个常见的坑）。把帧标成
   I 帧，再配合 `init()` 里的 `forced-idr=1` 选项，libx264 才会把它编成真正的 IDR，
   而不是普通 I 帧。

#### 解码错误隐藏检测 → onError → PLI

解码侧还有一个"优雅降级"机制。文件 `h264_decoder.cpp` 的 `decode()`：

```cpp
while (avcodec_receive_frame(codecCtx_, frame_) == 0) {
    if (decodedCb_) {
        decodedCb_(frame_->data[0], frame_->width, frame_->height);
    }
    // 错误隐藏检测：decode_error_flags 非零表示这帧是 FFmpeg "猜"出来的
    if (frame_->decode_error_flags != 0 && errorCb_) {
        errorCb_();
    }
}
```

当 P 帧损坏时，FFmpeg 不会报错，而是用错误隐藏技术"修补"出一帧，画面表现为花屏/绿屏。
`decode_error_flags != 0` 就代表这种"猜出来的帧"。此时触发 `onError` 回调，在
`main_client.cpp` 里接的是节流后的 `sendPli()`——于是解码器一发现花屏，就请求对端发关键帧。

### 代码精读

文件：`src/media/video/h264_encoder.h/.cpp`、`h264_decoder.h/.cpp`。

#### 编码器 init() 的参数配置（引用真实代码）

```cpp
bool H264Encoder::init() {
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        codec = avcodec_find_encoder(AV_CODEC_ID_H264);   // 回退到系统默认
    }
    if (!codec) {
        Logger::error("H264 encoder not found");
        return false;
    }
    codecCtx_ = avcodec_alloc_context3(codec);

    codecCtx_->bit_rate = config_.bitrateKbps * 1000;  // kbps → bps
    codecCtx_->width = config_.width;
    codecCtx_->height = config_.height;
    codecCtx_->time_base = {1, config_.fps};           // 1/fps 秒一个 PTS
    codecCtx_->framerate = {config_.fps, 1};
    codecCtx_->gop_size = config_.gopSize;             // 30
    codecCtx_->max_b_frames = 0;                       // 禁用 B 帧
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;   // SPS/PPS 提到 extradata

    av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codecCtx_->priv_data, "profile", "baseline", 0);
    av_opt_set(codecCtx_->priv_data, "forced-idr", "1", 0);

    int ret = avcodec_open2(codecCtx_, codec, nullptr);

    frame_ = av_frame_alloc();
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = config_.width;
    frame_->height = config_.height;
    av_frame_get_buffer(frame_, 0);
    return true;
}
```

`AV_CODEC_FLAG_GLOBAL_HEADER` 的作用值得单独说：它把 SPS/PPS 从"每个 IDR 前都带一份"
改成"放进 `codecCtx_->extradata`"。在 WebRTC 里 SPS/PPS 通常通过 SDP 的 `fmtp` 参数或
RTCP 里单独传给对端，而不是每个关键帧都重复携带。

#### 编码一帧：YUV 平面映射 + send/receive

```cpp
void H264Encoder::encode(const uint8_t* yuvData, size_t len) {
    int ySize = config_.width * config_.height;
    int uvSize = ySize / 4;                       // 4:2:0，色度各为亮度 1/4
    int expectedLen = ySize + 2 * uvSize;         // 总大小 = Y + U + V
    if (static_cast<int>(len) < expectedLen) return;

    frame_->data[0] = const_cast<uint8_t*>(yuvData);                  // Y
    frame_->data[1] = const_cast<uint8_t*>(yuvData + ySize);          // U
    frame_->data[2] = const_cast<uint8_t*>(yuvData + ySize + uvSize); // V
    frame_->linesize[0] = config_.width;
    frame_->linesize[1] = config_.width / 2;
    frame_->linesize[2] = config_.width / 2;
    frame_->pts = pts_++;

    if (forceKeyframe_) { ... AV_PICTURE_TYPE_I ... }
    else { ... AV_PICTURE_TYPE_NONE ... }

    if (avcodec_send_frame(codecCtx_, frame_) < 0) return;

    AVPacket* pkt = av_packet_alloc();
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        processPacket(pkt);        // 解析 AVCC 格式，逐个 NAL 回调
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}
```

YUV420P 平面布局记忆：`Y`(width×height) + `U`(width/2×height/2) + `V`(width/2×height/2)，
总大小 = `width * height * 3 / 2`（相比 RGB 的 3 字节/像素省一半）。

#### processPacket：AVCC → 裸 NAL

FFmpeg libx264 输出的是 **AVCC 格式**（`[4字节大端长度][NAL数据]...`），而 RTP 传输要
**裸 NAL**（不含长度前缀、不含起始码）。`processPacket()` 负责解析 AVCC：

```cpp
void H264Encoder::processPacket(AVPacket* pkt) {
    const uint8_t* data = pkt->data;
    int size = pkt->size;
    int offset = 0;
    while (offset < size) {
        if (offset + 4 > size) break;
        int nalSize = (data[offset] << 24) | (data[offset + 1] << 16) |
                      (data[offset + 2] << 8) | data[offset + 3];
        offset += 4;
        if (offset + nalSize > size) break;
        if (encodedCb_) encodedCb_(data + offset, nalSize);
        offset += nalSize;
    }
}
```

#### 解码：裸 NAL 加 Annex B 起始码后送解码器

解码端反过来：FFmpeg 解码器要的是 **Annex B 格式**（`00 00 00 01` 起始码 + NAL），所以
`h264_decoder.cpp` 的 `decode()` 先加起始码：

```cpp
void H264Decoder::decode(const uint8_t* nalData, size_t nalLen) {
    if (!initialized_) return;
    std::vector<uint8_t> annexB;
    annexB.reserve(4 + nalLen);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x00);
    annexB.push_back(0x01);
    annexB.insert(annexB.end(), nalData, nalData + nalLen);

    pkt_->data = annexB.data();
    pkt_->size = static_cast<int>(annexB.size());

    if (avcodec_send_packet(codecCtx_, pkt_) < 0) {
        if (errorCb_) errorCb_();   // 码流损坏 → 触发 onError
        return;
    }
    while (avcodec_receive_frame(codecCtx_, frame_) == 0) {
        if (decodedCb_) decodedCb_(frame_->data[0], frame_->width, frame_->height);
        if (frame_->decode_error_flags != 0 && errorCb_) errorCb_();
    }
}
```

编码端输出"裸 NAL"、解码端加"起始码"，两端正好拼成一个闭环。这是 AVCC 与 Annex B 两种
封装格式的典型分工（面试常考）。

### 动手练习

**练习 4.1（观察首个 NAL 类型）：** 跑 `./crystal_demo` 的 Demo 4，观察编码输出的第一个
NAL 是什么类型？为什么？（答：通常是 SPS/PPS，编码器初始化后第一次输出会带 SPS/PPS；
紧接着是 IDR，因为首帧必须能独立解码。）

**练习 4.2（改 GOP）：** 把 `h264_encoder.cpp`（或 config）里的 `gop_size` 从 30 改成 5，
重新编译运行，观察 IDR 帧出现的频率变高（关键帧更密、码率略升）。

**练习 4.3（打印 NAL 类型）：** 在 `processPacket()` 里把每个 NAL 首字节的低 5 位打印出来，
连续编码多帧，观察 SPS/PPS/IDR/P 的出现规律（SPS/PPS 只在开头出现，IDR 每隔 gop 出现一次）。

### 面试高频题

1. **为什么实时通话不用 B 帧？** B 帧要参考后续帧才能解码，需要缓冲和重排，引入编码延迟；
   实时通话对延迟敏感，用 I/P 帧即够（本项目干脆 `max_b_frames=0`）。
2. **SPS 和 PPS 的作用？** SPS 含分辨率、帧率、Profile 等序列级参数；PPS 含熵编码模式、
   参考帧数等图像级参数。解码器必须先拿到它们才能解 IDR。
3. **为什么用 baseline profile？** 无 B 帧、无 CABAC，延迟最低、兼容性最好，是最安全的
   实时通信选择。
4. **IDR 帧和普通 I 帧有什么区别？** IDR 会清空参考帧缓冲，后续帧只能从它开始预测；
   普通 I 帧仍可引用旧参考帧。恢复花屏必须用 IDR，所以编码端开了 `forced-idr`。
5. **FFmpeg 的 send/receive 模型为什么这样设计？** 因为一次 send（喂一帧/包）不保证立即
   产出一个完整输出，编码/解码内部有缓冲，所以要"尽力 send、循环 receive 取空"。

---

## Module 5：Opus 音频编解码

### 概念讲解

Opus 是 WebRTC 的**强制（Mandatory）音频编解码器**，所有符合标准的 WebRTC 实现都必须支持。
它横跨语音和音乐两类场景，内部是**混合编码器**：

| 模式 | 适用 | 目标码率 | 技术基础 |
|------|------|---------|---------|
| SILK | 语音 | 6~40 kbps（低码率） | 线性预测（LPC），利用语音准周期性 |
| CELT | 音乐/混合 | 40~510 kbps（中高码率） | MDCT 变换编码，类似 MP3/AAC |
| Hybrid | 中码率过渡 | 约 24~32 kbps | SILK 管低频 + CELT 管高频 |

Opus 编码器会根据信号特征和码率**自动选择**最优模式，应用层无需干预。

#### 为什么 WebRTC 选 Opus

- **码率范围极宽**：6~510 kbps，能适配从窄带语音到高质量音乐；
- **帧长灵活**：2.5ms~60ms，满足不同延迟/质量取舍；
- **低延迟**：算法延迟小，适合实时；
- **自带 DTX（静音抑制）和 PLC（丢包隐藏）**：这两点对实时通话特别实用（下文详述）。

#### 关键参数表

| 参数 | 本项目取值 | 含义与影响 |
|------|-----------|-----------|
| `application` | `OPUS_APPLICATION_VOIP` | 语音优化（另可选 AUDIO 音乐、RESTRICTED_LOWDELAY 极低延迟） |
| `bitrate` | 64 kbps | 目标码率，Opus 是 VBR，实际围绕此值波动 |
| `DTX` | 1（开） | 静音时只发极小的"舒适噪声"帧，节省约 50% 带宽 |
| `complexity` | 5（0~10） | 编码复杂度，越高越慢越省码率，5 是平衡点 |
| `frameSize` | 960（48kHz × 20ms） | 每帧采样点，决定算法延迟 |

**帧长计算（面试常考的计算题）：** `frameSize = sampleRate × frameDuration`。
48kHz 下 20ms → `48000 × 0.02 = 960` 采样点；输入 PCM 是 int16，即 `960 × 2 = 1920 字节`。

#### DTX（Discontinuous Transmission）是什么

DTX 是静音抑制：当检测到静音/背景噪声时，编码器不按正常码率编码，而是输出约 1~2 字节的
"舒适噪声（comfort noise）"帧。接收端解码这几个字节后生成一段与背景噪声接近的信号，
避免"突然死寂"的突兀感。典型语音通话约 50% 时间是静音，DTX 能省约一半带宽。

#### 音频为什么不重传（NACK）——对比视频

这是贯穿项目的关键设计权衡，面试大概率追问：

- **视频**：一帧丢了会导致后续整串 P 帧解不出（参考帧链断裂），所以值得用 NACK 重传或
  直接 PLI 要关键帧。
- **音频**：**Opus 自带 PLC（Packet Loss Concealment，丢包隐藏）**，丢一帧时它可以根据
  前一帧"插值"出一段近似信号，人耳几乎听不出。而且音频重传到达时，往往已经**错过了播放
  时刻**（该播的 20ms 窗口早过去了）。所以音频**不做 NACK**，让它用 PLC 掩盖，更划算。

对应到代码：`main_client.cpp` 里视频 PT=96 走了完整的 NACK 流程，音频 PT=97 只做统计、
不触发 NACK。

### 代码精读

文件：`src/media/audio/opus_encoder.h/.cpp`、`opus_decoder.h/.cpp`。

#### 编码器 init()：参数设置

```cpp
bool OpusEncoder::init() {
    int error;
    encoder_ = opus_encoder_create(config_.sampleRate, config_.channels,
                                    OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || !encoder_) {
        Logger::error("Failed to create Opus encoder: {}", opus_strerror(error));
        return false;
    }
    // 目标码率
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrateKbps * 1000));
    // 复杂度 0~10，5 是平衡点
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));
    // 静音抑制
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
    return true;
}
```

#### 编码一帧

```cpp
std::vector<uint8_t> OpusEncoder::encode(const int16_t* pcmData, int frameSize) {
    if (!encoder_) return {};
    std::vector<uint8_t> output(4000);   // 单帧最大约 4000 字节的保守上限
    int len = opus_encode(encoder_, pcmData, frameSize,
                          output.data(), static_cast<opus_int32>(output.size()));
    if (len < 0) {
        Logger::warn("Opus encode failed: {}", opus_strerror(len));
        return {};
    }
    output.resize(len);   // Opus 是 VBR，缩到实际长度
    return output;
}
```

注意 `output` 先申请 4000 字节（保守上限），编码后再 `resize(len)` 到实际长度——因为 Opus
是**可变码率**，每帧长度不同（语音活跃帧约 40~160 字节，DTX 舒适噪声帧只有 1~2 字节）。

#### 解码一帧（含 PLC 概念）

```cpp
std::vector<int16_t> OpusDecoder::decode(const uint8_t* opusData, size_t len,
                                         int frameSize) {
    if (!decoder_) return {};
    std::vector<int16_t> output(frameSize * channels_);
    int samples = opus_decode(decoder_, opusData, static_cast<opus_int32>(len),
                              output.data(), frameSize, 0);
    if (samples < 0) {
        return {};
    }
    output.resize(samples * channels_);
    return output;
}
```

注意 `opus_decode` 的**最后一个参数是 `decode_fec`**，本项目传的是 `0`（即"正常解码，
不主动用 FEC"）。当这个参数非 0 时，Opus 会用**带内前向纠错（in-band FEC）** 来恢复上一帧
丢失的数据。这引出了一个进阶话题（综合练习里会展开）：

- **PLC（丢包隐藏）**：丢包帧用"上一帧的推测"补，是解码器内部的隐藏，不需要额外数据；
- **FEC（前向纠错）**：编码时把上一帧的低码率副本"夹带"在当前帧里，解码时可用它恢复上一帧，
  代价是**额外带宽**（约增加 20~30%）。本项目当前没开 FEC。

### 动手练习

**练习 5.1（改码率看帧大小）：** 把 `opus_encoder.cpp` 里的 `OPUS_SET_BITRATE` 从
64kbps 改到 16kbps，重新编译运行（或在编码后 `print` 帧大小），观察单帧输出字节数变小。

**练习 5.2（改应用类型）：** 把 `OPUS_APPLICATION_VOIP` 改成 `OPUS_APPLICATION_AUDIO`，
思考什么场景该用哪个（语音通话用 VOIP，音乐共享/会议用 AUDIO）。

**练习 5.3（观察 DTX）：** 保持 DTX=1，静音对着麦克风说话，观察编码输出是否变成 1~2 字节
的舒适噪声帧（可加日志打印帧大小）。

### 面试高频题

1. **Opus 为什么比 AAC 更适合实时通话？** Opus 有 DTX、低延迟、极宽码率范围（6~510kbps）
   与自动模式切换（SILK/CELT），且是 WebRTC 强制编解码器。
2. **DTX 是什么？** Discontinuous Transmission 静音抑制：静音时只发约 1~2 字节舒适噪声帧，
   典型通话可省约一半带宽。
3. **20ms 帧是什么含义？甄计算。** 每 20ms 编一次码，48kHz 下即 960 采样点；int16 PCM
   输入为 `960 × 2 = 1920` 字节。
4. **音频为什么不做 NACK，而视频做？** 视频丢一帧会拖垮后续整串参考帧，值得重传；Opus
   自带 PLC 丢包隐藏，且重传到时已错过播放窗口，掩盖更划算。
5. **PLC 和 FEC 的区别？** PLC 是解码器内部用上一帧推测补缺（零额外带宽）；FEC 是编码时
   把上一帧低码率副本夹带在当前帧里（要额外带宽），解码时可精确恢复上一帧。

---

## Module 6：信令

### 概念讲解

WebRTC 标准**故意不定义信令协议**——两个 Peer 必须先通过某种"第三方通道"互相交换连接信息，
这个通道用什么协议由应用自己定（SIP、Jingle、自定义 JSON over WebSocket 都行）。交换的核心
内容有两样：

1. **SDP（Session Description Protocol）** —— 描述媒体能力（编码格式、分辨率、ICE 参数、
   DTLS 指纹等）。
2. **ICE Candidate** —— 网络地址信息（IP + 端口 + 候选类型）。

信令服务器就是这个"第三方通道"。

#### 为什么 WebRTC 不定义信令协议

因为不同场景需求差异太大，且信令属于"应用层编排"，与媒体传输解耦可以让 WebRTC 保持轻量
和灵活。有的应用用 SIP 对接运营商，有的用 Jingle 对接 XMPP，本项目用一个简单的
JSON-over-WebSocket 服务器。

#### 协议设计表（以实际代码为准）

本项目信令消息是 JSON，通过 WebSocket 传输。消息类型与方向（见 `signaling_server.h` 注释）：

| 类型 | 方向 | 关键字段 | 作用 |
|------|------|---------|------|
| `join` | 客户端 → 服务器 | `room` | 加入房间 |
| `joined` | 服务器 → 客户端 | `peerId` | 确认加入成功，分配 peerId |
| `peer_joined` | 服务器 → 房间内其他人 | `peerId` | 通知有新人加入 |
| `offer` | 客户端 ↔ 客户端（经服务器转发） | `sdp`, `to`, `from` | 发起方发 SDP Offer |
| `answer` | 客户端 ↔ 客户端 | `sdp`, `to`, `from` | 被叫方回 SDP Answer |
| `candidate` | 客户端 ↔ 客户端 | `candidate`, `sdpMid`, `sdpMLineIndex`, `to`, `from` | 交换 ICE 候选 |
| `leave` | 客户端 → 服务器 | — | 离开房间 |
| `peer_left` | 服务器 → 房间内其他人 | `peerId` | 通知有人离开 |

（原 `SignalingMessage` 结构体里其实没有 `error` 类型字段，消息类型取值在 `signaling_server.h`
里写的是 `join/offer/answer/candidate/leave/joined/peer_joined/peer_left` 这一组。）

#### 信令时序（面试必画，可对照 `signaling_client.h` 注释）

```
客户端A            信令服务器            客户端B
  │   join(room) ──────►│                    │
  │ ◄── joined ─────────│                    │
  │                     │◄── join(room) ─────│
  │ ◄── peer_joined ────│─── peer_joined ──►│
  │   offer(B) ────────►│─── offer(A) ─────►│
  │                     │◄── answer(A) ─────│
  │ ◄── answer(B) ──────│                    │
  │   candidate(B) ────►│─── candidate(A)──►│
  │ ◄── candidate(A) ───│◄── candidate(B) ──│
  ═══════════════ P2P 直连建立 ══════════════
```

**关键理解（反复考）：** 信令服务器只转发消息，**不碰媒体数据**。P2P 连接一旦建立，
音视频直接在对端之间传输，不再经过服务器。

#### 服务器逻辑：房间表、转发 vs 广播

- `signaling_server.cpp` 的 `handleMessage()` 里：
  - `join`：把 peer 加入房间表，向房间**广播** `peer_joined`，并**单独**回 `joined` 给本人；
  - `offer`/`answer`/`candidate`：都是**点对点转发**（带上 `from` 字段后发给 `to` 指定的 peer）；
  - `leave`：从房间表移除，向房间广播 `peer_left`。
- 区分"定向发送 `sendToPeer`" 和 "广播 `broadcastToRoom`" 是理解消息路由的关键。

#### 客户端状态机：谁先发 offer

在 `main_client.cpp` 里，**后进房间的人收到 `peer_joined` 之后作为发起方发 offer**：

```cpp
signalingClient.onMessage([&](const crystal::SignalingMessage& msg) {
    if (msg.type == "peer_joined") {
        auto sdp = pc->createOffer();                 // 我当发起方
        signalingClient.sendOffer(sdp, msg.peerId);
    } else if (msg.type == "offer") {
        pc->setRemoteDescription(msg.sdp, "offer");   // 被叫方：设远端，回 answer
        auto answer = pc->createAnswer();
        signalingClient.sendAnswer(answer, msg.peerId);
    } else if (msg.type == "answer") {
        pc->setRemoteDescription(msg.sdp, "answer");  // 发起方：设远端，完成
    } else if (msg.type == "candidate") {
        pc->addIceCandidate(msg.candidate, msg.sdpMid, msg.sdpMLineIndex);
    }
});
```

**ICE candidate 乱序问题**：SDP 和 candidate 是**两个独立的信令消息**，可能出现
"candidate 先于 SDP 到达"的情况。工业级实现要用 trickle ICE 的候选缓存机制（把过早到达的
候选先缓存，等远端描述设置好再注入）。本项目简化处理——`main_client` 里 candidate 直接
`addIceCandidate`，没有做显式缓存，这是一个可以追问的"简化点"（面试诚实话术见后文）。

### 代码精读

文件：`src/signaling/signaling_client.h/.cpp`、`signaling_server.h/.cpp`。

#### 服务器：消息路由（handleMessage 核心逻辑）

```cpp
void SignalingServer::handleMessage(const std::string& peerId, const std::string& data) {
    json j = json::parse(data);
    SignalingMessage msg;
    msg.type = j.value("type", "");
    msg.sdp = j.value("sdp", "");
    msg.candidate = j.value("candidate", "");
    msg.sdpMid = j.value("sdpMid", "");
    msg.sdpMLineIndex = j.value("sdpMLineIndex", 0);
    msg.to = j.value("to", "");
    msg.room = j.value("room", "");
    msg.peerId = peerId;

    if (msg.type == "join") {
        peers_[peerId].room = msg.room;
        // 广播 peer_joined 给房间其他人，单独回 joined 给本人
        json notify;
        notify["type"] = "peer_joined";
        notify["peerId"] = peerId;
        broadcastToRoom(msg.room, notify.dump(), peerId);
        json welcome;
        welcome["type"] = "joined";
        welcome["peerId"] = peerId;
        sendToPeer(peerId, welcome.dump());
    } else if (msg.type == "offer" || msg.type == "answer" || msg.type == "candidate") {
        if (!msg.to.empty()) {
            json forward = j;
            forward["from"] = peerId;         // 转发时标上真实来源
            sendToPeer(msg.to, forward.dump());
        }
    } else if (msg.type == "leave") {
        std::string room = peers_[peerId].room;
        peers_.erase(peerId);
        json notify;
        notify["type"] = "peer_left";
        notify["peerId"] = peerId;
        broadcastToRoom(room, notify.dump(), "");
    }
}
```

要点：
- 服务器把 `peerId` 填入消息（`msg.peerId = peerId`），转发 `offer/answer/candidate` 时
  **追加 `from` 字段**并填原始 peerId，这样接收端才知道回给谁。
- `broadcastToRoom(..., peerId)` 的第三个参数是"排除谁"，广播 `peer_joined` 时排除**新人本人**
  （避免给自己发"有人加入"），广播 `peer_left` 时排除空串（给所有人发）。

#### 客户端：WebSocket 连接 + 消息解析

```cpp
bool SignalingClient::connect(const std::string& url, uint16_t port) {
    struct lws_context_creation_info ctxInfo;
    memset(&ctxInfo, 0, sizeof(ctxInfo));
    ctxInfo.port = CONTEXT_PORT_NO_LISTEN;
    context_ = lws_create_context(&ctxInfo);
    // ... 配置 lws_client_connect_info（address/port/path/protocol=...）
    wsi_ = lws_client_connect_via_info(&ccInfo);
    if (!wsi_) { lws_context_destroy(context_); return false; }
    connected_ = true;
    serviceThread_ = std::thread(&SignalingClient::serviceThread, this);  // 独立事件循环线程
    return true;
}
```

关键设计：
- **libwebsockets 事件循环跑在独立线程** `serviceThread`（循环调 `lws_service`），
  所以非阻塞的 `connect` 返回后，消息收发都在那个线程里完成。
- 收到的消息通过 `handleMessage()` 解析成 `SignalingMessage`，再回调给上层 `onMessage`。
- 在 `handleMessage` 里，若消息类型是 `joined`，把服务器分配的 `peerId` 存到 `peerId_`：
  `if (msg.type == "joined") peerId_ = msg.peerId;`（`signaling_client.cpp` 里如此）。

### 动手练习

**练习 6.1（观察消息序列）：** 启动服务器 + 两个客户端，看双方日志里 `joined`、
`peer_joined`、`offer`、`answer`、`candidate` 出现的顺序，和上文时序图对照。

```bash
./build/crystal_signaling_server &
./build/crystal_client room1 127.0.0.1 8765   # 终端 1
./build/crystal_client room1 127.0.0.1 8765   # 终端 2
```

**练习 6.2（断线重连观察）：** 强杀其中一个客户端，观察另一个客户端是否收到 `peer_left`
？（提示：需要服务器正确处理 WebSocket 断开事件 `unregisterPeer`，可自己加日志验证。）

### 面试高频题

1. **为什么 WebRTC 不定义信令协议？** 让应用按场景自由选择（SIP/Jingle/自定义），WebRTC
   只聚焦媒体传输，信令作为应用层编排与媒体解耦。
2. **SDP Offer/Answer 模型是什么？** 发起方发 Offer（包含全部媒体能力），被叫方回 Answer
   （在 Offer 基础上选择双方共同支持的参数）。
3. **ICE Candidate 有哪几种类型？** Host（本机地址）、SRFLX（STUN 反射地址）、Relay
   （TURN 中继地址），另有 PRFLX（对端反射地址）。本项目 ICE 概念详见 Module 7。
4. **信令服务器转发和广播的区别？** `offer/answer/candidate` 是点对点转发（带上 `from`），
   `peer_joined/peer_left` 是房间内广播。
5. **心跳与重连怎么做？信令安全如何保证？** 心跳通过应用层周期消息维持 WSocket 连接活性；
   重连需要在断线后重新 join 并重协商；安全上生产环境应使用 WSS（TLS）+ 鉴权。本项目是
   明文、无鉴权的教学实现。

---

## Module 7：传输层 ICE/DTLS/SRTP

### 概念讲解

传输层解决"两个可能在各自 NAT 后面的 Peer，怎么建立一条安全加密的直连通道"。分成三步：

```
1. ICE  (Interactive Connectivity Establishment)  —— 找到可用网络路径（打洞）
2. DTLS (Datagram TLS)                            —— 在 UDP 上握手，导出密钥
3. SRTP (Secure RTP)                              —— 用 DTLS 密钥加密媒体包
```

#### ICE：为什么需要，四种候选是啥

两端通常都在 NAT 后面，不知道自己的公网地址，也不知道对方能通过什么地址连到自己。ICE 的
思路是**收集尽可能多的候选地址，两边配对做连通性检查，挑能用且优先级最高的**。

| 候选类型 | 全称 | 来源 | 场景 |
|---------|------|------|------|
| Host | 本地地址 | 网卡直配的 IP:Port | 同一局域网直连 |
| SRFLX | Server Reflexive | STUN 服务器反射出的公网地址 | 一方/双方在普通 NAT 后 |
| PRFLX | Peer Reflexive | 对端在做连通性检查时学到的地址 | 对称 NAT 的补充 |
| Relay | 中继地址 | TURN 服务器分配 | 双方都在对称 NAT 后，最后手段 |

优先级通常是 Host > SRFLX > PRFLX > Relay（越靠前越"直连"、延迟越低）。需要 STUN/TURN 的
原因：STUN 帮你发现自己经 NAT 映射后的公网 IP（"我在别人眼里是谁"）；TURN 在直连彻底打不通
时做中继兜底（"帮你转交数据"）。**对称 NAT** 场景（相同内网地址访问不同目标会得到不同映射）
STUN 无法可靠穿透，必须用 TURN。

对应代码在 `ice_config.h`：`IceConfig` 里有 `stunServers`/`turnServers`/`turnUsername`
/`turnPassword`，`defaultConfig()` 返回一个 Google 公共 STUN（仅开发测试用）。

#### DTLS-SRTP：为什么是 DTLS 而不是 TLS，密钥从哪里来

- TLS 基于 TCP，UDP 上不能用；DTLS 是"跑在 UDP 上的 TLS"，额外处理了丢包和乱序。
- 握手完成后，DTLS 会把导出的密钥材料交给 SRTP 使用（**DTLS-SRTP，RFC 5764**），
  媒体加密密钥**不需要额外协商**，直接从握手结果导出。这样"认证"和"加密"复用同一次握手，
  简单且安全。

#### libdatachannel 封装了什么

本项目传输层不自己实现 ICE/DTLS/SRTP，而是用 **libdatachannel**（一个 C++ WebRTC 传输库）。
`transport_manager.cpp` 里的 `PeerConnection` 类封装了它。libdatachannel 帮我们做的事：

- 配置 ICE 服务器（`rtc::Configuration` / `iceServers`）；
- 创建 `rtc::PeerConnection` 并注册回调（`onLocalCandidate`/`onStateChange`/`onGatheringStateChange`）；
- 管理媒体 `Track`（`addTrack`/`onMessage`/`send`）；
- 完成 SDP 生成（`localDescription`）与远端描述设置（`setRemoteDescription`）。

### 代码精读

文件：`src/transport/transport_manager.h/.cpp`、`ice_config.h`。

#### 构造：ICE 配置 + 回调注册

```cpp
PeerConnection::PeerConnection(const IceConfig& config) : config_(config) {
    rtc::InitLogger(rtc::LogLevel::Warning);
    rtc::Configuration rtcConfig;
    for (const auto& stun : config_.stunServers) {
        rtcConfig.iceServers.emplace_back(stun);
    }
    for (const auto& turn : config_.turnServers) {
        rtcConfig.iceServers.emplace_back(turn, 3478,
                                           config_.turnUsername,
                                           config_.turnPassword);
    }
    pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);

    pc_->onLocalCandidate([this](const rtc::Candidate& cand) {
        if (iceCandidateCb_) {
            iceCandidateCb_(std::string(cand.candidate()),
                           std::string(cand.mid()), 0);
        }
    });

    pc_->onStateChange([this](rtc::PeerConnection::State state) {
        if (stateCb_) stateCb_(state);
    });

    pc_->onGatheringStateChange([](rtc::PeerConnection::GatheringState state) {
        Logger::debug("ICE gathering state: {}", static_cast<int>(state));
    });
}
```

注意点：
- `onLocalCandidate` 每发现一个本机候选就通过回调通知应用层，应用层再经信令发给对端。
- `onStateChange` 的 `State::Connected` 表示 **ICE 连通 + DTLS 握手都完成**，可以发数据了：
  `isConnected()` 判断的就是 `pc_->state() == rtc::PeerConnection::State::Connected`。

#### createOffer / createAnswer

```cpp
std::string PeerConnection::createOffer() {
    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    auto track = pc_->addTrack(video);
    if (track) {
        track_ = track;
        installTrackHandler();    // 装 RTP/RTCP 去复用回调
    }
    auto desc = pc_->localDescription();
    if (desc) {
        return injectRtcpFb(std::string(*desc));  // 注入 rtcp-fb 能力声明后返回
    }
    return "";
}
```

`Direction::SendRecv` 表示双向媒体。`createAnswer()` 同理，拿到 `localDescription` 后也走
`injectRtcpFb`。

#### setRemoteDescription

```cpp
void PeerConnection::setRemoteDescription(const std::string& sdp,
                                           const std::string& type) {
    rtc::Description desc(sdp, type == "answer" ? rtc::Description::Type::Answer
                                                : rtc::Description::Type::Offer);
    pc_->setRemoteDescription(desc);
    if (!track_) {
        pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
            track_ = track;
            installTrackHandler();   // 被叫路径：远端 Track 到达同样装回调
        });
    }
}
```

设置远端描述后，libdatachannel 会解析对端 ICE 参数（ufrag/pwd）、开始连通性检查、启动
DTLS 握手。被叫方（还没有 `track_`）在这里注册 `onTrack` 以接住远端的媒体轨道。

#### installTrackHandler：RTP/RTCP 去复用（RFC 5761）

```cpp
void PeerConnection::installTrackHandler() {
    track_->onMessage([this](const rtc::binary& data) {
        auto bytes = binaryToVec(data);
        if (isRtcpPacket(bytes.data(), bytes.size())) {
            if (rtcpCb_) rtcpCb_(bytes);      // RTCP → 独立通道
        } else if (trackCb_) {
            trackCb_(bytes);                   // RTP → 媒体链路
        }
    }, nullptr);
}
```

`isRtcpPacket` 就是 Module 8 的去复用判定。`rtc::binary`（底层是 `std::vector<std::byte>`）
和 `std::vector<uint8_t>` 不能隐式互换，所以要 `binaryToVec`/`vecToBinary` 显式转换。

#### injectRtcpFb：SDP 字符串后处理的"为什么"

```cpp
static std::string injectRtcpFb(const std::string& sdp) {
    if (sdp.find("a=rtcp-fb:96") != std::string::npos) {
        return sdp;   // 已注入过，幂等
    }
    auto mpos = sdp.find("m=video");
    if (mpos == std::string::npos) return sdp;
    auto lineEnd = sdp.find('\n', mpos);
    if (lineEnd == std::string::npos) return sdp;
    const std::string fb = "a=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\n";
    std::string result = sdp;
    return result.insert(lineEnd + 1, fb);
}
```

为什么用**字符串后处理**而不是 libdatachannel API？因为 libdatachannel 的
`Description::Media` 没公开按 payload type 加 `a=rtcp-fb` 的接口。这里是直接往 `m=video`
行后插入两行声明属性的字符串。它是**声明性**的：本项目两端都是 CrystalRTC，PT=96 是约定值，
我方 NACK/PLI 收发其实不依赖 SDP 协商结果；但完整的 SDP 有利于将来和浏览器互操作（浏览器
只对 SDP 声明了 rtcp-fb 的 PT 发 NACK）。

### 动手练习

1. **观察 SDP 里注入的 rtcp-fb。** 在 `transport_manager.cpp` 的 `injectRtcpFb()` 返回前加一行
   `Logger::info("SDP after inject:\n{}", result)`（或 `printf`），跑一次 `crystal_client`，看
   `m=video` 行之后是不是刚好多了两行 `a=rtcp-fb:96 nack` 和 `a=rtcp-fb:96 nack pli`。
   再连跑两次，确认第二次没有重复注入——这验证的是"幂等"逻辑：函数开头
   `sdp.find("a=rtcp-fb:96") != npos` 会提前返回。

2. **打印 ICE 候选类型与顺序。** 在 `onLocalCandidate` 里把 `cand.candidate()` 原样打出来，
   观察候选字符串末尾的 `typ host`（内网地址）、`typ srflx`（STUN 反射出的公网地址）。
   注意出候选的顺序：host 最先、srflx 紧随其后，对应 ICE 的优先级：直连优于反射、
   反射优于中继。如果配置了 TURN，还会看到 `typ relay`。思考：为什么这个顺序是合理的？

3. **断网观察连接状态机。** 两端连通后，用 `iptables -A OUTPUT -p udp -j DROP`（或拔网线）造成
   断链，观察 `PeerConnection state:` 日志从 `Connected` 走到 `Disconnected` 再到 `Failed` 的
   整条路径。对照 `onStateChange` 注释里的状态机说明，回答："Connected 到底意味着什么？"
   （提示：注释里写的是 ICE 连通 + DTLS 握手都完成，不只是 ICE 通了。）

4. **手算去复用判定边界。** 在 `main_demo.cpp` 里调用 `isRtcpPacket` 验证：`{0x80, 200}` 第二字节
   200 ∈ [192,223]，应判 RTCP；`{0x80, 226}` 第二字节 226 在区间外，应判 RTP。再用真实收到的
   视频 RTP 包验证：M=1、PT=96 时第二字节 = 0x80 | 96 = 224，仍在 [192,223] 之外——这就是为什么
   本项目选 PT=96/97 能保证去复用判定"无歧义"。

### 面试高频题

1. **ICE 的全称和作用？** Interactive Connectivity Establishment，在 NAT 环境下通过收集
   Host/SRFLX/PRFLX/Relay 候选并做连通性检查，找到可用的网络路径。
2. **什么时候必须用 TURN？** 双方都在对称 NAT 后面，STUN 无法可靠穿透时，只能 TURN 中继。
3. **DTLS 和 TLS 的区别？** DTLS 基于 UDP、TLS 基于 TCP；DTLS 要处理丢包和乱序，握手增加了
   重传和防重放机制。
4. **SRTP 密钥从哪里来？** 从 DTLS 握手结果导出（DTLS-SRTP），不需要额外协商密钥。
5. **RTP 和 RTCP 怎么在同一个 Track 上区分？** RFC 5761：看第 2 字节，值 ∈ [192,223] 判为
   RTCP，否则是 RTP（本项目 PT=96/97，M+PT 组合落在区间外，无歧义）。

---

## Module 8：RTCP 反馈体系

### 概念讲解

RTP 只管"传"，网络质量谁管？交给 RTCP（RTP Control Protocol）。RTCP 让收发双方互相报告
网络状况，并支撑两件关键的"补救"工作：**重传（NACK）** 和 **请求关键帧（PLI）**。

先看一个完整的丢包恢复故事，建立直觉：

```
RTP:  [seq=100] [seq=102] [seq=103]      ← 101 丢了
                    │
                    ▼ 接收端 JitterBuffer 检测到间隙（Module 3 的 insert 返回值）
NACK:  "请重传 seq=101"                    ← 专用 RTCP 包（PID=101, BLP=0）
                    │
                    ▼ 发送端查重传缓冲，补发 seq=101 的原始字节
重传: [seq=101]
```

若重传了 3 次还是没回来（对端根本没收到 NACK 或重传也丢了），就放弃，转 PLI：

```
PLI: "给我个关键帧重新开始"  → 发送端下一帧强制编 IDR
```

#### 四种核心 RTCP 包

| 包 | 类型号（第2字节） | 作用 |
|----|------------------|------|
| SR（Sender Report） | 200 | 发送端声明：已发多少包/字节 + NTP/RTP 时间戳对（供算 RTT 和同步） |
| RR（Receiver Report） | 201 | 接收端反馈：丢包率/抖动/最高序号，每条流一个报告块 |
| NACK（RTPFB） | 205（FMT=1） | 接收端点名重传：FCI 放丢失 seq 的 PID + 16 位位图 |
| PLI（PSFB） | 206（FMT=1） | 接收端请求关键帧：解不出来，给我个 IDR |

（另有 SDES=202，仅实现 CNAME，复合包里要带。）

#### RTP/RTCP 复用同一端口，怎么区分（RFC 5761）

这是本项目的实际实现。看 RTP 头的**第 2 字节**：

- RTCP 包类型（第 2 字节） ∈ **[192, 223]**；
- RTP 的第 2 字节是 `M|PT`，本项目 PT=96/97，`M=1` 时是 224/225，`M=0` 时是 96/97，
  都落在这个区间**外面**，所以判定无歧义。

代码 `rtcp_packet.cpp`：

```cpp
bool isRtcpPacket(const uint8_t* data, size_t size) {
    return size >= 2 && data[1] >= 192 && data[1] <= 223;
}
```

#### RTT 怎么算：LSR/DLSR 法

全程**不需要两端时钟同步**（RFC 3550 A.6）。原理图解：

```
发送端 A 在 SR 里带 NTP 时间戳（记作 T1）
  → 接收端 B 收到，记录它自己的 LSR = 该 SR 的 NTP 中间 32 位
  → B 准备发 RR 时，填 DLSR = (此刻 - 收到 SR 时刻)（1/65536 秒单位）
  → A 收到 RR：RTT = (A 当前 NTP) - LSR - DLSR
```

关键点：`DLSR` 里抵消了 B 端的时钟基准，所以 A 端算出的是纯粹的网络往返时间。

Lsr 是"SR 的 NTP 时间戳的**中间 32 位**"，对应函数 `ntpMiddle32()`（`rtcp_packet.cpp`）。

#### NACK 与 PLI 的分工

| | NACK | PLI |
|---|------|-----|
| 触发 | 零星丢包（间隙 ≤ 64） | 大间隙 / 重传放弃 / 解码错误 |
| 手段 | 点对点重传原包 | 请求关键帧（IDR） |
| 代价 | 延迟换质量（重传要一个 RTT） | 带宽换恢复（关键帧大） |
| 兜底 | 重试 3 次未恢复则放弃 | 触发后 500ms 节流防风暴 |

**兜底链**：`NackRequester` 重试 3 次（每次间隔 33ms）仍没等到，就把该 seq 计入"放弃"；
`main_client` 检测到放弃计数增长，就发一次 PLI。解码器检测到花屏（`decode_error_flags`）
也会发 PLI。PLI 有 500ms 节流，防止"错误→PLI→大帧→更易丢→错误"的正反馈风暴。

### 代码精读

文件：`src/media/rtcp/rtcp_packet.h/.cpp`、`retransmission_buffer.h/.cpp`、
`nack_requester.h/.cpp`、`rtcp_reporter.h/.cpp`。这些 `.cpp` 现已自带详尽中文注释，
**读法建议：先读 `.h` 头部注释理解职责，再读 `.cpp` 里的函数块注释，最后看代码。**

#### NACK 的 PID + BLP 位图（buildEntries）

一个 FCI 条目 4 字节：`PID`(16bit) + `BLP`(16bit)。`BLP` 的第 `i` 位（0~15）表示
`PID + i + 1` 也丢了，所以**一个条目最多点名 17 个连续丢包**。

```cpp
std::vector<NackEntry> NackPacket::buildEntries(std::vector<uint16_t> lostSeqs) {
    if (lostSeqs.empty()) return {};
    // 以首元素为基准的模 65536 排序：正确处理回绕
    const uint16_t base = lostSeqs.front();
    std::sort(lostSeqs.begin(), lostSeqs.end(),
              [base](uint16_t a, uint16_t b) {
                  return static_cast<uint16_t>(a - base) <
                         static_cast<uint16_t>(b - base);
              });
    lostSeqs.erase(std::unique(lostSeqs.begin(), lostSeqs.end()), lostSeqs.end());

    std::vector<NackEntry> out;
    for (uint16_t s : lostSeqs) {
        if (out.empty() || static_cast<uint16_t>(s - out.back().pid) > 16) {
            out.push_back({s, 0});   // 距上一个 PID 超过 16 → 开新条目
        } else {
            out.back().blp |= static_cast<uint16_t>(1u << (s - out.back().pid - 1));
        }
    }
    return out;
}
```

**手算算例：** 假设丢失 `{100, 102, 103}`：

- 第一轮 `s=100`，`out` 空 → `out=[{100, 0}]`；
- `s=102`，距 PID 100 差 2（≤16）→ BLP 第 `2-1=1` 位置 1 → `BLP=0b0010=0x0002`；
- `s=103`，距 PID 100 差 3 → BLP 第 `3-1=2` 位再置 1 → `BLP=0b0110=0x0006`。
- 结果一个条目 `{pid=100, blp=0x0006}`，展开后就是 `{100, 102, 103}`。

为什么排序要用"以首元素为基准的模 65536 排序"？如果直接数值排序，`{65534, 65535, 0, 1}`
这种跨越回绕点的丢包集合会被拆散（0/1 排到最前面），无法合并成位图。用 `a - base` 的无符号
差值做比较，就能在"环绕"的坐标轴上保持正确顺序。

#### 复合包解析（parseRtcpCompound）

RTCP 常常是"复合包"——一个 UDP 负载里塞多个子包（比如 `RR + SDES`）。`parseRtcpCompound`
逐个解析：

```cpp
bool parseRtcpCompound(const uint8_t* data, size_t size,
                       const RtcpPacketHandler& handler) {
    size_t offset = 0;
    while (offset + 4 <= size) {
        const uint8_t* p = data + offset;
        if ((p[0] >> 6) != 2) return false;       // 版本必须 2
        uint8_t fmtOrCount = p[0] & 0x1F;
        uint8_t pt = p[1];
        size_t byteLen = (static_cast<size_t>(getU16(p + 2)) + 1) * 4;
        if (offset + byteLen > size) return false; // 长度越界 → 畸形，停

        // ... 按 pt 分派：SR/RR/SDES/RTPFB(NACK)/PSFB(PLI)，填 RtcpPacket ...
        if (pkt.kind != RtcpKind::Unknown && handler) handler(pkt);
        offset += byteLen;
    }
    return offset == size;   // 完整消费才算成功
}
```

读法要点：`length` 字段表示"整个子包字节数 / 4 − 1"，所以真实字节长度是 `(length+1)*4`；
`parseRtcpCompound` 对畸形包（版本错/长度越界）**立即停止**，返回值还要求"完整消费全部字节"
才算成功，这是防御性解析。

#### 重传缓冲（deque + TTL）

```cpp
void RetransmissionBuffer::store(uint16_t seq, std::vector<uint8_t> packet, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    entries_.push_back({seq, nowMs, std::move(packet)});
}

std::vector<uint8_t> RetransmissionBuffer::get(uint16_t seq, uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictLocked(nowMs);
    for (const auto& e : entries_) {
        if (e.seq == seq) { retransmitted_++; return e.data; }
    }
    miss_++;   // 已淘汰/从未存过
    return {};
}
```

双维度淘汰：容量（默认 512 包，超出丢最老）+ 时间（默认 3s TTL，包过"保鲜期"再被 NACK
就补发不上，直接计 miss）。默认构造 `RetransmissionBuffer(size_t capacity = 512,
uint64_t ttlMs = 3000)`。为什么用 3s？超过 3s 说明 RTT 很大 + 重试窗口都过去了，接收端肯定
已经放弃这个 seq，补发也没意义。**为什么不引入 RTX（RFC 4588）**？RTX 用专用 SSRC 重传，
需要额外协商；教学项目直接"原样补发原字节"、序列号不变，接收端 JitterBuffer 无感知。

#### NACK 状态机时间线

```cpp
class NackRequester {
public:
    static constexpr int kMaxRetries = 3;          // 含首次请求的总次数
    static constexpr uint64_t kRetryIntervalMs = 33;
    std::vector<uint16_t> onMissing(const std::vector<uint16_t>& gaps, uint64_t nowMs);
    std::vector<uint16_t> tick(uint64_t nowMs);
    void onReceived(uint16_t seq);
    uint64_t requestedCount() const;
    uint64_t givenUpCount() const;
private:
    std::map<uint16_t, Item> pending_;   // key = 丢失 seq
};
```

时间线：检测到新丢包 → `onMissing` 立即返回"该请求的 seq" → 若没恢复，`tick` 每 33ms
重试一次，超过 3 次 → 计入 `givenUpCount`（放弃，交给 PLI）。33ms 约等于 30fps 一帧周期；
3 次 × 33ms ≈ 100ms，超过这个窗口接收端缓冲早已输出该时间片，继续重传无意义。

#### rtcp_reporter：RTT 数值算例与抖动平滑

`RecvSideReporter` 抖动（RFC 3550 A.8）：

```cpp
// transit = 到达时刻(RTP单位) - RTP时间戳
// d = |transit_i - transit_{i-1}|
jitter_ += (d - jitter_) / 16.0;
```

除以 16 是**指数平滑**（增益 1/16），既跟得上抖动变化，又抗单次毛刺。这就是注释里说的
"平滑系数"。

`RecvSideReporter::buildBlock` 算丢包率和 fraction lost：

```cpp
uint32_t extMax = cycles_ + maxSeq_;              // 扩展最高序号（处理回绕）
uint64_t expected = extMax - baseSeq_ + 1;        // 期望应收包数
int64_t lost = static_cast<int64_t>(expected) - static_cast<int64_t>(received_);
```

然后 `fractionLost` 算的是**自上份报告以来的增量丢包率**（8 位线性映射 0~255 对应 0~100%）。

`SendSideReporter` 收到对面 RR 时算 RTT：

```cpp
// RTT = 收到 RR 的 NTP 时刻 - LSR - DLSR （RFC 3550 A.6）
```

对应头注释一句精华：**RTT 只能在发送侧算**，因为 LSR 是"对端收到我方 SR 的时刻"、DLSR 是
"对端从收 SR 到发 RR 的间隔"，两者之差被 NTP 对齐后才能减出网络往返时间。

### 动手练习

1. **观察 NACK 生效**：用 `tc netem` 注入 5% 丢包（命令见 USAGE.md），对照 `[stats]` 行——
   实际丢包率应明显低于注入值，说明重传在补包。
2. **改参数做实验**：把 `NackRequester` 的 `kMaxRetries` 从 3 改成 10，在 30% 丢包下观察
   "放弃"计数与画面恢复速度变化，体会"重传也丢"的边际效应。
3. **看协议字节**：在 `sendRtcp` 处加打印，dump NACK 包十六进制，对照 RFC 4585 逐字节验证
   V/P/PT/length 头与 FCI 的 PID/BLP。

### 面试高频题

1. **NACK 和 PLI 各适合什么场景？** NACK 适合零星丢包（重传延迟可控）；PLI 适合连续丢包
   或参考帧损坏——后续帧全部无法解码，只能要关键帧重新同步。
2. **RTT 为什么用 LSR/DLSR 而不是 ping？** 捎带在既有 SR/RR 里零额外带宽、测的就是媒体路径
   本身，且不要求两端时钟同步（DLSR 抵消 B 的时钟基准）。
   *追问：为什么 RTT 只能在发送侧算？* 因为 LSR/DLSR 描述的是"对端的时间间隔"，只有发 SR
   的一端拿到 RR 后，用自己当前的 NTP 减去它们，才能减出纯网络往返。
3. **RTP/RTCP 复用怎么区分？** RFC 5761：看第 2 字节，RTCP 类型 192-223；协商 RTP payload
   type 时避开这段区间防歧义。
4. **NACK 风暴怎么防？** ① JitterBuffer 大间隙（>64）不报，直接跳序；② NackRequester 状态机
   天然去重（同一 seq 只有一份待办）+ 重试上限；③ PLI 节流 500ms 防关键帧风暴（关键帧大会挤占带宽）。
5. **fraction lost 为什么只有 8 位？** 0-255 线性映射 0%-100%，报告块定长方便对齐解析；8 位
   足够表达"自上份报告以来"的增量丢包率，紧凑省空间。
   *追问：累计丢了多少怎么存？* 累计丢包 `cumulativeLost` 用 24 位（最大 0xFFFFFF）单独存。

---

## Module 9：采集与渲染

### 概念讲解

这是数据流的两端：把现实世界的声光变成数字（采集），把数字变回声光（渲染/播放）。虽然它们
和"协议"关系不大，但面试里经常考"底层是怎么拿到帧的、为什么这样设计线程模型"。

#### V4L2 视频采集流程

V4L2（Video for Linux 2）是 Linux 内核的摄像头统一接口，操作全靠 `open` + `ioctl`。
`v4l2_capture.cpp` 的流程：

```
open(/dev/video0)
  → VIDIOC_S_FMT    设置格式（分辨率、像素格式 YUV420）
  → VIDIOC_REQBUFS  申请缓冲区（MMAP 模式）
  → VIDIOC_STREAMON 启动采集流
  → 循环：VIDIOC_DQBUF 出队取帧 → 回调 → VIDIOC_QBUF 入队还缓冲
  → VIDIOC_STREAMOFF 停止
  → close()
```

**为什么 mmap 而不是 read？** V4L2 有三种缓冲模式：

| 模式 | 特点 |
|------|------|
| `V4L2_MEMORY_MMAP` | 内核分配缓冲，用户空间通过 `mmap` 映射访问，**零拷贝**，性能最优（本项目用这个） |
| `V4L2_MEMORY_USERPTR` | 用户空间分配，内核直接写入，灵活但要处理页对齐 |
| `V4L2_MEMORY_OVERLAY` | 直接写显存，已过时 |

DQBUF/QBUF 是"出队/入队"的循环：从内核拿一帧到用户空间（DQBUF），处理完把缓冲区还回去
（QBUF）让内核继续往里写。`open()` 时用 `O_NONBLOCK`，`DQBUF` 返回 `EAGAIN` 表示还没新帧，
捕获线程 `sleep(1ms)` 后重试。

#### ALSA 音频采集

ALSA 是 Linux 音频子系统。核心概念：

- **PCM 设备**：采集（capture）和回放（playback）两种；
- **period（周期）**：硬件每次中断传递的数据块，`snd_pcm_hw_params_set_period_size_near`
  把 period 设为 frameSize（960 采样点 = 20ms）。period 越小延迟越低但中断越频繁。
- **为什么会 glitch（爆音/卡顿）**：当用户态读取速度跟不上硬件写入速度，环形缓冲区
  溢出/欠载，就会丢采样产生爆音。因此采集线程要持续 `snd_pcm_readi`，读失败要
  `snd_pcm_recover` 恢复。

`alsa_capture.cpp` 里 `open()` 设置访问模式 `SND_PCM_ACCESS_RW_INTERLEAVED`（交错存取）、
格式 `SND_PCM_FORMAT_S16_LE`（16 位有符号小端）、声道、采样率（`_set_rate_near` 就近匹配，
不保证精确）、period 大小。

#### SDL 视频渲染

`SDL_UpdateYUVTexture` 接受 YUV 输入，由 GPU 自动做 YUV→RGB 转换，比 CPU 转换再渲染高效。
流程：`SDL_UpdateYUVTexture` → `SDL_RenderClear` → `SDL_RenderCopy` → `SDL_RenderPresent`。
纹理用 `SDL_PIXELFORMAT_IYUV`（即 YUV420P）+ `SDL_TEXTUREACCESS_STREAMING`（允许频繁更新）。

**为什么 SDL 事件要轮询？** SDL 是事件驱动的，窗口关闭、键盘、调整大小都要通过事件循环
处理。`pollEvents()` 必须频繁调用，否则窗口"无响应"。这就是 `main_client` 主循环里
`renderer.pollEvents()` 的原因，它还顺带兼任了 RTCP 周期任务的驱动。

#### SDL 音频播放：回调式拉取模型

这是最容易考的设计之一。SDL 音频用**拉模式（Pull Model）**：

- 应用层 `SDL_OpenAudio` 打开设备并注册回调 `audioCallback`；
- SDL 内部有音频线程，按固定频率**回调**你，向你要 PCM 数据（填 `stream`）；
- 你要做的是"被拉时给数据"，而不是"主动推数据"给设备。

```cpp
static void audioCallback(void* userdata, uint8_t* stream, int len);
```

**为什么用回调而不是推送？** 因为音频必须**严格按采样率节奏**输出，硬件时钟是主导方；
让 SDL 在"需要下一块数据"时回调你填缓冲，能精确对齐硬件节奏，避免应用层定时不准导致的
underrun（欠载）。本项目用 `std::queue<int16_t>` 做生产者（解码线程 `play()` push）与消费者
（SDL 音频线程 `fillBuffer()` pop）的桥梁，队列空时填 0（静音）。

### 代码精读

文件：`src/media/video/v4l2_capture.h/.cpp`、`sdl_renderer.h/.cpp`、
`src/media/audio/alsa_capture.h/.cpp`、`sdl_audio_player.h/.cpp`。

#### V4L2 open 与采集循环

```cpp
bool V4L2Capture::open() {
    fd_ = ::open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) return false;

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;  // 设备可能只支持 MJPEG
        if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) return false;
    }

    struct v4l2_requestbuffers req = {};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) < 0) return false;
    return true;
}
```

采集循环（独立线程）：

```cpp
void V4L2Capture::captureLoop() {
    buffer_.resize(config_.width * config_.height * 3 / 2);
    while (capturing_) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) { /* 非暂时性错误则警告 */ }
            usleep(1000);
            continue;
        }
        if (frameCb_ && buf.bytesused > 0) {
            size_t expectedSize = config_.width * config_.height * 3 / 2;
            if (buf.bytesused >= static_cast<unsigned int>(expectedSize)) {
                frameCb_(buffer_.data(), expectedSize);
            }
        }
        ioctl(fd_, VIDIOC_QBUF, &buf);
    }
}
```

#### ALSA open

```cpp
bool AlsaCapture::open() {
    int err = snd_pcm_open(&pcm_, config_.device.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) return false;

    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm_, params);
    snd_pcm_hw_params_set_access(pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm_, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm_, params, config_.channels);
    unsigned int rate = config_.sampleRate;
    snd_pcm_hw_params_set_rate_near(pcm_, params, &rate, nullptr);
    snd_pcm_uframes_t frames = config_.frameSize;
    snd_pcm_hw_params_set_period_size_near(pcm_, params, &frames, nullptr);
    err = snd_pcm_hw_params(pcm_, params);
    if (err < 0) { snd_pcm_close(pcm_); pcm_ = nullptr; return false; }
    return true;
}
```

#### SDL 音频回调填充

```cpp
void SDLAudioPlayer::fillBuffer(uint8_t* stream, int len) {
    std::lock_guard<std::mutex> lock(mutex_);
    int16_t* out = reinterpret_cast<int16_t*>(stream);
    int samples = len / 2;              // len 是字节数，int16_t 每个 2 字节
    for (int i = 0; i < samples; i++) {
        if (!buffer_.empty()) {
            out[i] = buffer_.front();
            buffer_.pop();
        } else {
            out[i] = 0;                 // 欠载：填静音，避免爆音
        }
    }
}
```

回调在 SDL 音频线程里执行，所以要加 `mutex_`（与解码线程的 `play()` 竞争）。回调必须**尽快
返回**，不能做阻塞操作（文件 I/O、网络请求等）——这是 SDL 音频回调的铁律。

### 动手练习

1. **观察 ALSA period 与 Opus 帧的对齐。** 在 `alsa_capture.cpp` 的 `captureLoop()` 里加一条日志，
   打印 `snd_pcm_readi` 每次实际返回的帧数。正常应该稳定在 960（48kHz × 20ms）。如果偶尔偏小，
   思考为什么（提示：`snd_pcm_hw_params_set_period_size_near` 可能因为声卡约束返回了
   非 960 的 period，或者读取被信号打断）。这个"每帧 960 采样"与 `opus_encoder.cpp` 强制要求的
   frameSize 必须一致，否则编码会错位——这是音频链路里最容易踩的坑之一。

2. **观察 DTX 静音帧。** 在不说话时看 `opus_encoder.cpp` 的 `encode()` 返回的 `len`：说话时约
   40~160 字节，静音时因为 init 里 `OPUS_SET_DTX(1)` 启用了不连续传输，会掉到 1~2 字节的舒适
   噪声帧。用一条日志把每帧编码后的字节数打出来，对着麦克风说话/闭嘴观察字节数的跳变。

3. **制造音频欠载看静音填充。** 在 `sdl_audio_player.cpp` 的 `play()` 里人为少喂数据（比如
   把入队数量减半），听扬声器输出：缓冲区空了之后 `fillBuffer()` 会填 0，表现为短暂静音而非爆音。
   这正是"欠载时用静音掩盖"的工程手段。再思考：如果改成填上一次的最后一个采样（保持波形），
   效果会如何？（这是更高级的波形拼接/PCM 掩盖思路，音频领域常叫"重复上一帧"。）

4. **验证 YUV 平面偏移。** 在 `sdl_renderer.cpp` 的 `render()` 里，注释里给了三个平面指针的偏移：
   Y 平面从 0 开始，U 平面从 `width*height` 开始，V 平面从 `width*height*5/4` 开始。手算一遍
   640×480 的 YUV420P：Y = 640×480 = 307200 字节，U = 320×240 = 76800，V 同 U。三个偏移分别是
   0、307200、384000，总和 460800 = 640×480×3/2。用计算器验证后，就不容易再被"YUV 占多少内存"
   这种基础题问倒。

### 面试高频题

1. **V4L2 mmap 的原理？** 内核分配帧缓冲，用户空间通过 `mmap` 映射同一块物理内存，实现
   零拷贝；配合 DQBUF/QBUF 出队/入队循环，内核和用户空间交替使用缓冲。
2. **音频缓冲欠载（underrun）会怎样？如何处理？** 数据供给跟不上播放时，产生爆音/断续；
   处理：增大缓冲、填静音掩盖（本项目队列空时填 0），更优解是用环形缓冲区降低开销。
3. **SDL 渲染线程模型？** 主线程轮询 `pollEvents()` 处理事件，渲染在 `render()` 里通过
   GPU 硬件加速完成 YUV→RGB 转换。
4. **为什么 SDL 音频用回调（拉模式）而不是推送？** 音频要严格按硬件采样率节奏输出，让 SDL
   回调"按需拉数据"能精确对齐硬件时钟，避免应用层定时不准；回调里只填缓冲、不做重活。
5. **V4L2 采集线程为什么用 `O_NONBLOCK` + `EAGAIN` 循环？** 非阻塞模式下拿不到帧时不会卡死
   线程，可以 `sleep` 后重试，便于用 `capturing_` 标志优雅退出。

---

## Module 10：工程化

### 概念讲解

这里讲"这个项目怎么组织、怎么编译、怎么测试、怎么调试"，是面试中体现"工程素养"的部分。

#### CMake 分层结构

根 `CMakeLists.txt` 做三件事：拉依赖、找系统库、定义可执行文件。

拉依赖用 `FetchContent`（版本锁定）：

```cmake
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.14.1)
FetchContent_Declare(libdatachannel GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git GIT_TAG v0.21.2)
FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.14.0)
FetchContent_MakeAvailable(spdlog googletest libdatachannel)
```

**版本锁定的意义**：`GIT_TAG` 钉住具体版本，避免上游更新导致构建不可复现。这是一个工程好习惯。

系统库用 `pkg_check_modules` 找 SDL/FFmpeg/Opus/ALSA：

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(ALSA REQUIRED alsa)
```

**为什么用 `pkg_check_modules` 而不是 `find_package`？** 因为这些库主要提供 `.pc` 文件
（由 pkg-config 管理），`pkg_check_modules` 能直接读它们的头文件路径、库路径和链接选项，
比手写 `find_package` 模块省事且通用。

#### 静态库分层（src/CMakeLists.txt）

`src/CMakeLists.txt` 把每个模块编成独立静态库，形成依赖层次：

```
               ┌── crystal_media_rtp   ── 依赖 crystal_utils
               │── crystal_media_rtcp  ── 依赖 crystal_utils
crystal_utils ─┤
（日志，地基）   │── crystal_media_video ── 依赖 crystal_utils + crystal_media_rtp + FFmpeg/SDL
               │── crystal_media_audio ── 依赖 crystal_utils + crystal_media_rtp + Opus/ALSA/SDL
               │── crystal_transport   ── 依赖 crystal_utils + crystal_media_rtcp + libdatachannel
               │── crystal_signaling   ── 依赖 crystal_utils + nlohmann_json
               └── crystal_room        ── 依赖 crystal_utils + nlohmann_json
```

要点：`crystal_media_video` / `crystal_media_audio` 都依赖 `crystal_media_rtp`（编解码产物要
交给 RTP 打包/解包），`crystal_transport` 依赖 `crystal_media_rtcp`（传输层要做 RTP/RTCP 去复
用判定，需要 `isRtcpPacket`）。注意 `crystal_media_rtcp` **不**依赖 `crystal_media_rtp`——RTCP
的反馈报文是独立定义的协议结构。这个"谁依赖谁"的边，恰恰是理解模块边界的最佳线索。

典型一行：

```cmake
add_library(crystal_media_rtp STATIC media/rtp/rtp_packet.cpp media/rtp/rtp_packetizer.cpp
            media/rtp/rtp_depacketizer.cpp media/rtp/jitter_buffer.cpp)
target_include_directories(crystal_media_rtp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_rtp PUBLIC crystal_utils)
```

分层的收益：**模块独立可测**（tests 里可只链接 `crystal_media_rtp`）、**依赖清晰**。

#### --whole-archive 的作用（面试加分题）

根 `CMakeLists.txt` 里链接 `crystal_client` 时，把自研静态库用 `--whole-archive` 包了起来：

```cmake
add_executable(crystal_client main_client.cpp)
target_link_libraries(crystal_client PRIVATE
    -Wl,--whole-archive
    crystal_signaling crystal_transport crystal_media_video crystal_media_audio
    crystal_media_rtp crystal_room crystal_utils
    -Wl,--no-whole-archive
    datachannel websockets spdlog::spdlog nlohmann_json::nlohmann_json
    ${SDL2_LIBRARIES}
    avcodec swresample swscale avutil
    opus asound
)
```

这里的 `-Wl,` 是把后面的选项原样传给链接器（`ld`）。`--whole-archive` 和
`--no-whole-archive` 中间夹的自研静态库，会被**整个链接进来**。

为什么需要它？先理解静态库（`.a`）的默认行为：一个 `.a` 本质是一堆 `.o` 目标文件的打包，
链接器**只抽取"定义了当前缺失符号"的那个 `.o`**。如果某个 `.o` 里的所有符号都没被
`main` 直接或间接引用，它就会被整段丢弃。

对本项目来说，`main_client.cpp` 里其实**直接**构造了几乎所有类（`V4L2Capture`、
`H264Encoder`、`NackRequester`……），理论上每个 `.o` 都会被抽出来。但负责任的工程做法
仍然是显式 `--whole-archive`：它保证了"模块完整链接"这件事不依赖"碰巧每个 `.o` 都被
引用"这个脆弱前提——将来任何人把某个模块改成"只被回调间接使用"，也不至于悄悄被链接器
丢符号，造成诡异的运行时 undefined symbol 或空实现。

一句话记法：**默认是"按需抽取"，`--whole-archive` 是"整库都要"**。面试能说出这一句，
再加一个"某 `.o` 无人引用就被丢"的例子，就是加分回答。

### 代码精读

本模块的"代码"就是三个 `CMakeLists.txt` + 一个日志封装，文件都不长，但值得逐块读。

#### 根 CMakeLists：FetchContent 拉依赖（版本锁定）

```cmake
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.21.2
)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(BUILD_TESTING ON)
FetchContent_MakeAvailable(spdlog googletest libdatachannel)
```

`GIT_TAG` 钉死具体 commit/tag，保证任何人、任何时候 clone 下来构建结果一致（可复现构建）。
这是团队协作里最容易被忽略却最重要的一件事：不锁定版本，三个月后上游升级就可能编译失败。

#### 根 CMakeLists：可执行目标与 --whole-archive

`crystal_demo`、`crystal_signaling_server`、`crystal_client` 三个可执行文件分别只链接自己
需要的库。`crystal_demo` 是"无硬件依赖的演示"，所以只链 video/rtp/utils + FFmpeg，**不链**
ALSA/V4L2/SDL 音频；`crystal_signaling_server` 只链 signaling + websockets。这种"最小链接"说明
作者对模块边界有清晰认知。

#### src/CMakeLists：静态库分层 + include 目录

每个 `add_library(... STATIC ...)` 后面都跟着两句固定套路：

```cmake
target_include_directories(crystal_media_rtp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_rtp PUBLIC crystal_utils)
```

`PUBLIC` 的意思是"传播"：靠 `crystal_media_rtp` 的库，会自动获得 `${CMAKE_CURRENT_SOURCE_DIR}`
这个头文件搜索路径，也能看到 `crystal_utils` 的链接依赖。这就是为什么源码里能直接
`#include "media/rtp/rtp_packet.h"`——根目录被加进了 include path。

#### tests/CMakeLists：两批测试可执行文件

```cmake
add_executable(crystal_rtp_tests rtp_packet_test.cpp rtp_packetizer_test.cpp
    rtp_depacketizer_test.cpp jitter_buffer_test.cpp)
target_link_libraries(crystal_rtp_tests PRIVATE
    crystal_media_rtp crystal_utils GTest::gtest_main)
add_test(NAME crystal_rtp_tests COMMAND crystal_rtp_tests)
```

`add_test` 把可执行文件注册给 CTest，之后 `ctest` 就能统一跑。RTP 测试只链 `crystal_media_rtp`
+ `crystal_utils`，RTCP 测试额外链 `crystal_media_rtcp`（和 `crystal_media_rtp`，因为
`jitter_buffer_gap_test` 用到了 JitterBuffer）——这再次体现了分层带来的"可独立测试"红利。

#### utils/logger：一行 spdlog 封装

`Logger::init("client")` 之后，整个程序用 `Logger::debug/info/warn/error` 输出。前端是
spdlog 的便利性（`{}` 占位符、按等级过滤），后端是统一的名字和前缀。读陌生代码时，
**先看 `Logger::init` 设了什么等级**，就能控制自己能看到的日志量。

### 动手练习

1. **全程编译一遍并复现。** 在一个干净目录执行 `cmake -S . -B build` 然后 `cmake --build build`。
   观察 FetchContent 第一次拉取的日志（会 clone spdlog/libdatachannel/googletest），读懂构建
   步骤后，你就知道"别人机器上跑不起来"大概率是缺系统库（SDL/FFmpeg/Opus/ALSA）还是网络问题。

2. **跑单测。** 进入 `build` 目录执行 `ctest --output-on-failure`。看哪些测试通过、用 `--verbose`
   看每个用例名。然后故意把 `rtp_packet.cpp` 里序列化代码改坏一行（比如把某个字节写错），重新
   `cmake --build build` 后 `ctest` 应能立刻定位到失败用例——体会"单测是安全的改动前提"。

3. **验证 --whole-archive 的效果。** 把根 `CMakeLists.txt` 里 `crystal_client` 的
   `-Wl,--whole-archive` 和 `-Wl,--no-whole-archive` 两行暂时注释掉，重新链接。观察是否出现
   `undefined reference`（如果 main 都直接引用了，可能不报错，这正好印证"按需抽取"其实已经够用，
   但 --whole-archive 提供了防御）。写一句总结：这个实验让你对"静态链接"有了第一手体感。

4. **调日志等级。** 把 `Logger::init("client")` 附近的日志等级从 info 改成 debug，重跑，观察
   ICE candidate、每帧编码等 debug 日志刷屏；再改成 warn，观察日志几乎清空。理解"日志等级是
   调试时的第一把旋钮"。

### 面试高频题

1. **`--whole-archive` 是干什么的？** 强制链接器把整个静态库的所有 `.o` 都链接进来，而非默认的
   "只抽取定义了缺失符号的 `.o`"。得分点：能说出".a 是 .o 的打包，默认按需抽取"。
2. **为什么用 `FetchContent` 而不是 `git submodule` 或 `add_subdirectory`？** FetchContent 在
   配置阶段按 `GIT_TAG` 拉取并纳入构建，无需手动 clone 子模块，且版本锁定保证可复现。得分点：
   "版本可复现"。
3. **`pkg_check_modules` 和 `find_package` 有什么区别？** `pkg_check_modules` 走 pkg-config 的
   `.pc` 文件（SDL/FFmpeg/Opus/ALSA 都提供），自动拿 include/库路径；`find_package` 走 CMake
   模块/配置文件。得分点：知道什么库适合哪种方式。
4. **单测为什么要独立成可执行文件？** 每个目标只链自己需要的库，粒度小、编译快、失败易定位，
   也反向约束了模块边界（能独立测说明耦合低）。得分点："分层可独立测试"。
5. **如何读一个陌生 C++ 项目的代码？** 先看根 `CMakeLists.txt` 摸清模块划分和依赖，再顺着
   `main` 的一条数据流走，配合日志和注释。得分点：展示方法论而非只背结论。

---

## 面试专题

这一章是前面 11 个模块的"出口"：把散落的知识点，按面试官真实的提问方式重组。建议在学完
Module 0~10 之后、面试前一周再精读一遍。

### (a) 项目自我介绍模板（STAR 结构）

STAR = Situation（背景）/ Task（任务）/ Action（动作）/ Result（结果）。本项目天然适配。

**30 秒版（电梯演讲，面试开场"介绍下你的项目"）：**

> "我做过一个 C++ 自研的 WebRTC P2P 音视频通话项目 CrystalRTC。它不依赖浏览器，用纯
> `libdatachannel` 做传输、FFmpeg 做 H.264、libopus 做音频，自己实现了 RTP 打包/解包、
> 抖动缓冲、以及 NACK/PLI/SR/RR 一整套 RTCP 反馈。两端通过 WebSocket 信令交换 SDP 和 ICE
> 候选，最终走 DTLS-SRTP 加密的 P2P 直连。最花心思的是接收端的丢包恢复：JitterBuffer 检
> 测丢包 → NACK 状态机请求重传 → 重传耗尽再用 PLI 请求关键帧兜底。"

**3 分钟版（展开讲）：**

S（Situation）："实时音视频对丢包和延迟很敏感，UDP 不保证送达，必须自己解决乱序、丢包、
抖动三件事。"

T（Task）："我想亲手把 WebRTC 的媒体链路从采集到渲染写一遍，而不是只会调浏览器 API。"

A（Action，按模块串）：
> "发送端：V4L2/ALSA 采集 → H.264/Opus 编码 → 自己写 RTP 打包器（大 NAL 做 FU-A 分片）→
> libdatachannel 的 SRTP 加密发送。接收端：先按 RFC 5761 去复用 RTP/RTCP → JitterBuffer
> 用双指针重排乱序、检测丢包 → NACK（PID+BLP 位图）请求重传 → PLI 请求关键帧。控制面
> RTCP 的 SR/RR 算 RTT 和丢包率，每 5 秒打一条统计。"

R（Result）："在 5% 丢包下通过 NACK 重传恢复画面，30% 丢包下退化为 PLI 保底。跑了 9 个单元
测试，覆盖 RTP/RTCP/抖动缓冲/NACK 状态机等核心路径。通过这个项目，我把 RFC 3550/6184/4585
的协议细节落到了代码里。"

### (b) 高频追问清单（≥30 条，按主题分类）

每条都标了"答什么得分"——面试官要的不是背定义，而是"能解释为什么、能讲数值"。

#### RTP（6 条）

1. **RTP 头 12 字节有哪些字段、各自作用？** 得分：V/P/X/CC/M/PT/SeqNum/Timestamp/SSRC 全
   说对，且能说"SeqNum 防乱序、Timestamp 音画同步、SSRC 区分流、M 标帧边界、PT 标编码"。
2. **为什么媒体走 UDP + RTP，不用 TCP？** 得分：实时性优先，TCP 重传会放大延迟；丢一帧
   视频比等一秒强。顺带说 RTCP 做质量反馈是 UDP 的补位。
3. **序列号 65535→0 回绕怎么比较？** 得分：`int16_t diff = (int16_t)(seq - expected)`，
   利用补码溢出，回绕前后 diff 都正确；给一个数值例子。
4. **为什么视频时钟 90000Hz、音频 48000Hz？** 得分：90000 是常见视频帧率（24/25/30/60）
   的公倍数，能整除出整数 timestamp 增量；48000 是 Opus 的标准采样率。
5. **RTP 和 RTCP 什么关系？** 得分：RTP 传数据，RTCP 传控制反馈，同端口复用（RFC 5761），
   互为补充。
6. **SSRC 为什么要随机？** 得分：全局唯一标识一路流；随机降低两路流碰撞概率、提高安全性
   （防已知明文/预测攻击）。

#### 编解码（6 条）

7. **I/P/B 帧区别，实时通话为什么不用 B 帧？** 得分：I 帧自包含、P 帧参考前向、B 帧双向参考；
   B 帧引入编码延迟且依赖未来帧，实时场景禁用。
8. **为什么实时用 baseline + zerolatency + ultrafast？** 得分：baseline 无 B 帧/CABAC（低延迟）、
   zerolatency 关编码缓冲、ultrafast 换取编码速度；三者都在"以压缩率换低延迟"。
9. **FFmpeg 的 send_frame/receive_packet 为什么是异步双队列？** 得分：编码不是一进一出，
   允许内部攒帧和延迟，send/receive 解耦、可重试，是生产级 API 的通用模式。
10. **SPS/PPS 为什么必须 IDR 之前？** 得分：SPS/PPS 是解码全局参数，没有它们 IDR 也解不了；
    IDR 丢了怎么办——PLI 请求强制重发关键帧（含 SPS/PPS）。
11. **NAL、Annex B、AVCC 的区别？** 得分：NAL 是逻辑单元，Annex B 用起始码分隔（网络/文件），
    AVCC 用长度前缀分隔（MP4）；本项目编码器输出 AVCC 再转裸 NAL。
12. **关键帧 I 帧为什么大？PLI 为什么节流？** 得分：I 帧是 P 帧数倍体积；无节流的"丢→PLI→
    大帧→更易丢"会正反馈风暴，所以 main_client 给 PLI 加了 500ms 节流。

#### 抖动缓冲（5 条）

13. **Jitter Buffer 解决哪三件事？** 得分：乱序重排、抖动吸收、丢包检测。
14. **为什么需要两个序列号指针？** 得分：`expectedSeq_` 只给 insert 做丢包检测、
    `nextOutputSeq_` 只给 consume 做输出排序；共用一个会让 consume 回退指针污染丢包统计。
15. **丢包检测里 diff 为什么就等于丢包数？** 得分：`expectedSeq_` 语义是"下一个期望收到的包"，
    `seq - expectedSeq_` 即跳过的包数；举例 expected=101 收 103 → 丢 2 个。
16. **为什么用 std::map 存包而不是 unordered_map/vector？** 得分：map 自动按 seq 排序便于按序
    输出，且能容纳丢包造成的键不连续。
17. **为什么音频不做 NACK？** 得分：重传到达时播放时刻已过；Opus 自带 PLC 丢包隐藏，静音掩盖
    比晚到重传划算；视频才有参考帧链必须补。

#### RTCP（7 条）

18. **NACK 的 PID+BLP 怎么压缩？** 得分：PID 是基准 seq，BLP 16 位每一位代表 PID+i+1 也丢，
    一条目最多表 17 个连续丢包，大幅省字节；给一个手算例子。
19. **NACK 和 PLI 分工？** 得分：NACK 是"小范围精确重传"，PLI 是"整个参考帧链断了、重发关键帧"；
    本项目 NACK 重试 3 次放弃后 PLI 兜底。
20. **RTT 怎么用 LSR/DLSR 算？为什么只能发送侧算？** 得分：`RTT = 收RR时刻 - LSR - DLSR`；
    LSR 是"对端收我 SR 的时刻"，DLSR 是"对端收 SR 到发 RR 的间隔"，这些只有发送侧才知道。
21. **SR 和 RR 区别？什么时候发纯 RR？** 得分：SR 带发送统计+NTP 锚点，RR 只有接收报告块；
    只收未发的一端发纯 RR（RFC 3550 6.4），本项目用 `videoSent` 标志区分。
22. **RFC 5761 去复用怎么看第二字节？** 得分：第二字节（PT 字段）∈ [192,223] 判 RTCP；本
    项目 PT=96/97 + M 位最大 225 落区间外，无歧义。
23. **为什么 gap > 64 不上报 NACK？** 得分：超大间隙意味着码流中断/重连，逐个请求会 NACK 风暴；
    画面恢复交给 PLI。这是"用域知识换工程稳健"的典型。
24. **抖动 jitter 怎么平滑？** 得分：RFC 3550 A.8，`jitter += (D - jitter)/16`，指数平滑，
    既跟得上波动又抗毛刺；除以 16 即增益 1/16。

#### 传输层（5 条）

25. **ICE 四种候选类型各怎么来？** 得分：host=本机网卡，srflx=STUN 反射出的公网地址，
    prflx=对端学到的地址，relay=TURN 中继地址。
26. **DTLS 和 TLS 区别？** 得分：DTLS 跑在 UDP 上，要处理丢包/乱序/重放，握手带重传和
    epoch 机制。
27. **SRTP 密钥从哪来？** 得分：DTLS-SRTP，从 DTLS 握手导出，不需要额外密钥协商。
28. **对称 NAT 为什么 STUN 打不穿、必须 TURN？** 得分：对称 NAT 给不同目的地分配不同映射
    端口，STUN 问到的地址只对 STUN 服务器有效，对端无法复用。
29. **libdatachannel 封装了什么？** 得分：ICE/DTLS/SRTP 全栈，对外只给 PeerConnection/Track/
    Description 等高层 API；本项目外加 `rtc::binary` 与 `uint8_t` 的转换封装。

#### 信令（4 条）

30. **为什么 WebRTC 标准不定义信令协议？** 得分：信令是"业务层"的事，标准只定媒体格式和
    传输，信令留给应用自由选（WebSocket/SIP/XMPP），保持解耦。
31. **offer/answer 模型怎么走？** 得分：呼叫方 offer（含能力），被叫方 answer（选择共同支持
    参数），双方各自 setRemoteDescription 后开始 ICE/DTLS。
32. **ICE candidate 会不会比 SDP 先到？怎么办？** 得分：会，trickle ICE 允许边协商边收候选；
    实现上要能在 setRemoteDescription 之前就先缓存候选。
33. **信令服务器为什么不碰媒体？** 得分：媒体走 P2P，签名只做信令转发，这是 WebRTC 的核心
    设计——省服务器带宽、保证端到端加密。

#### 工程化（4 条）

34. **`--whole-archive` 作用？** 得分：整库链接而非按需抽取 `.o`，见 Module 10。
35. **FetchContent 版本锁定含义？** 得分：`GIT_TAG` 钉死版本，保证可复现构建。
36. **`pkg_check_modules` vs `find_package`？** 得分：前者走 pkg-config 的 `.pc` 文件，适合
    SDL/FFmpeg/Opus/ALSA 这类只发 `.pc` 的库。
37. **你觉得自己哪里简化了、和真实 WebRTC 差在哪？** 得分：见下面 (c)，这是"诚实+"的必答题。

### (c) 弱点预演：诚实话术（必答）

面试官几乎一定会问："你这个和真正的 WebRTC 差在哪？" 答得诚实、精准，反而是加分项。下面把
本项目的简化点逐个列清，并给出"诚实 + 补救"的话术模板：

| 简化点 | 真实 WebRTC 怎么做 | 你会怎么补救 |
|--------|-------------------|--------------|
| 无拥塞控制 | GCC（Google Congestion Control）动态调码率 | 可接上自研的丢包率→码率反馈闭环 |
| 无带宽估计 | 基于丢包+延迟的带宽估计 | 用现有 RR 丢包率做第一版 ABR |
| 无 RTX 重传格式 | 用 RTX payload type 做重传 | 本项目直接原样补发原始 RTP 包 |
| JitterBuffer 简化 | NetEQ 有时间驱动、PLC、加速/减速播放 | 已有 targetDelayMs 参数但未用，可补时间驱动 |
| 无音画同步 | 用 RTCP SR 的 NTP 锚点做 lip-sync | 现有 SR/RR 已算出 NTP↔RTP 映射，可扩展 |
| 两方房间 | SFU/MCU 多方 | 现有 room + mesh 可扩展为 SFU |
| 无多路复用/重传统计 | 完整 RTCP 各类反馈 | 已实现 SR/RR/NACK/PLI 子集 |

诚实话术模板：

> "我的目标是理解骨架，所以有意做了取舍：没有实现 GCC 拥塞控制、带宽估计和 RTX，JitterBuffer
> 也简化成了序列号驱动的 drop 模式。但我知道这些缺失**分别对应真实系统的哪一块**——比如
> 拥塞控制可以用我已有的 RR 丢包率作为输入来做第一版 ABR，JitterBuffer 的 `targetDelayMs`
> 参数已经留了位，只是还没做时间驱动的延迟释放。这些如果要补齐，我有清晰的落地路径。"

这个回答把"我不会"转化成了"我知道边界在哪、也知道怎么补"，是区分度和可信度的来源。

### (d) 弱网实验数据话术（先自己跑出数据）

面试前，一定自己用 `tc netem` 跑出真实数据，再把下面这套话术背下来：

```bash
# 在一端加上 5% 丢包 + 20ms 抖动（对 UDP 生效的模拟）
sudo tc qdisc add dev lo root netem loss 5% delay 20ms
# 更狠的 30%
sudo tc qdisc replace dev lo root netem loss 30%
# 完事清理
sudo tc qdisc del dev lo root
```

观察 `[stats]` 行（每 5s 一条），字段含义：

```
[stats] 丢包 vX% aX% | 抖动 vXms aXms | RTT Xms | 重传 X miss X | NACK X 放弃 X
```

学习话术：

> "在 5% 丢包下，我的 `[stats]` 行显示 NACK 请求了几十次、重传补发成功，画面基本流畅，说明
> NACK 状态机 + 重传缓冲这条链路是工作的；丢包加到 30% 后，`放弃`（givenUp）计数持续上升，
> 因为 NACK 重试 3 次无法在播放窗口前完成，自动退化为 PLI 请求关键帧，画面降级为卡顿但不出
> 花屏——这验证了'小丢包靠 NACK、大丢包靠 PLI'的分层恢复策略。"

**关键点**：自己先跑一遍拿到真实数字再背，面试被追问具体百分比时才能对答如流。"我测过"永远
比"理论上"可信。

---

## 综合练习

### 练习 A：端到端数据流追踪

在 `main_client.cpp` 中，追踪一帧视频从摄像头到对方屏幕的完整路径：

```
V4L2Capture.onFrame(yuvData)
  → H264Encoder.encode(yuvData)
    → onEncoded(nalData)
      → RtpPacketizer.packetizeH264(nal)
        → PeerConnection.sendMedia(rtpData)
          → [网络传输]
            → PeerConnection.onTrack(data)
              → RtpPacket.parse(data)
                → JitterBuffer.insert(pkt)
                  → JitterBuffer.consume()
                    → RtpDepacketizer.depacketizeH264(pkt)
                      → H264Decoder.decode(nal)
                        → SDLRenderer.render(yuvData)
```

**任务：** 在每个回调函数里加一行 `Logger::info`，标上序号，运行 `crystal_client` 观察一帧
数据流的完整路径和触发顺序。重点体会：发送链是"采集线程→编码→打包→发送"的 push，接收链是
"onTrack 回调→按需 consume 拉取"的 pull。

### 练习 B：模拟丢包

修改 `main_demo.cpp` 的 Demo 4（编码→RTP→解码全管道），在发送端和接收端之间随机丢弃 10% 的
RTP 包：

```cpp
// 在发送循环中添加
if (rand() % 10 == 0) continue;   // 10% 概率丢包
```

观察：解码是否还能工作？画面会怎样？再逐步加到 30%、50%，记录"从哪一档开始花屏"。思考：
为什么丢关键帧（IDR）比丢 P 帧的影响大得多？（提示：P 帧依赖前面，IDR 是整条参考链的根。）

### 练习 C：实现 STAP-A

当前 `RtpPacketizer::packetizeH264` 只实现了单 NAL 和 FU-A。尝试实现 STAP-A 聚合模式：把
SPS+PPS+IDR 头打包进一个 RTP 包，减少小 NAL 各自开销。

提示：STAP-A 的格式为 NAL type=24（在 FU Indicator 的位置）+ 每个 NAL 的"2 字节长度 + 数据"，
依次拼接：

```
type=24 | NAL1长度(2B) | NAL1数据 | NAL2长度(2B) | NAL2数据 | ...
```

对应地，`RtpDepacketizer::depacketizeH264` 也要能识别 type=24 并拆回多个 NAL。参考 RFC 6184
Section 5.7.1。

### 练习 D：调 NACK 的 gap 阈值（弱网实验）

`jitter_buffer.h` 里的 `kMaxNackGap = 64` 决定"多小的间隙才上报 NACK"。把它改成 8 和 512 各
编译一次，配合 `tc netem loss 20%` 观察 `[stats]` 行里 NACK/放弃计数的变化：

- 阈值调小：大间隙（如断流 100 包）不再上报，画面恢复更依赖 PLI。
- 阈值调大：连大间隙都逐个请求，可能制造 NACK 风暴、挤占带宽。

记录两组数据，写一段"为什么 64 是个合理默认值"的分析（提示：正常抖动不会连续丢几十个包，
大间隙几乎必然意味着码流中断）。

### 练习 E：给 RTCP 加字节 dump

在 `main_client.cpp` 的 `pc->onRtcp` 回调开头，把收到的 RTCP 复合包前 16 字节以十六进制打印
出来（收到 NACK/PLI/RR 时各看一次）。对照 `rtcp_packet.h` 头注释里各包类型的第二字节取值
（SR=200/RR=201/SDES=202/RTPFB=205/PSFB=206），验证"第二字节就是包类型"这句话，并回答：
一个 NACK 复合包的第一字节和第二字节分别是什么？（提示：第一字节是版本 2 + padding + count。）

### 练习 F：打开 Opus FEC（调研任务）

先回答一个问题：**当前 `opus_encoder.cpp` 的 `init()` 有没有启用 FEC？**（答案：没有，它只
设置了 bitrate/complexity/DTX；FEC 需要 `OPUS_SET_INBAND_FEC(1)`，而且 `opus_decoder.cpp` 的
`decode()` 里 `opus_decode` 的最后一个 `decode_fec` 参数被写死为 0，也没有走 FEC 解码路径。）

所以这是一个**调研任务**，而不是"改一行就行"：

1. 查 libopus 文档搞清 `OPUS_SET_INBAND_FEC` 和 `OPUS_SET_PACKET_LOSS_PERC` 两个参数各自干什么；
2. 设计一个"打开 FEC 后，音频弱网丢包下的听感对比"实验步骤（用 `tc netem` + 录音回放）；
3. 写出改造方案：编码端如何周期性把 FEC 参数设为与预测丢包率匹配、解码端如何检测丢帧并
   用 `decode_fec=1` 恢复。不必实现，只要方案和理由清晰即可。

这个练习的价值在于：逼你把"Opus 为什么会自带 PLC 所以音频不用 NACK"这条结论，向前再推一步——
除了 PLC，Opus 还有 FEC 这种"主动冗余"手段，二者是"丢包后掩盖"和"丢包前预防"的关系。

---

## 推荐阅读

| 资源 | 看什么 |
|------|--------|
| RFC 3550 (RTP) | RTP 头字段、RTCP SR/RR、抖动/RTT 算法的原始权威定义 |
| RFC 4585 (RTCP FB) | NACK/PLI 等传输层、负载层反馈的报文格式 |
| RFC 5761 (Multiplexing) | RTP/RTCP 同端口复用的判定规则（本项目去复用的出处） |
| RFC 6184 (H.264 over RTP) | 单 NAL/FU-A/STAP-A 三种打包模式完整定义 |
| RFC 8445 (ICE) | ICE 四种候选、连通性检查、NAT 穿透的权威描述 |
| RFC 5764 (DTLS-SRTP) | DTLS 握手如何导出 SRTP 密钥 |
| webrtcforthecurious.com | 用通俗语言讲 WebRTC 全链路，最适合与本书对照 |
| High Performance Browser Networking | TCP/UDP/WebRTC 章节，补网络底层直觉 |
| libdatachannel 源码 | 看 ICE/DTLS/SRTP 的工程级实现，理解封装边界 |
| FFmpeg 官方文档 | libavcodec 的 send/receive API、x264 参数详解 |