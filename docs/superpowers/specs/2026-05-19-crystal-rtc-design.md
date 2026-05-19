# CrystalRTC — WebRTC C++ 渐进式通信项目设计文档

## 项目定位

CrystalRTC 是一个基于 C++ 的渐进式 WebRTC 通信项目，从 P2P 音视频通话起步，逐步扩展为 SFU 多人会议系统。项目面向 WebRTC C++ 岗位面试，展示对 WebRTC 核心协议、媒体管道、服务端架构的深度理解。

## 技术选型

| 模块 | 技术 | 理由 |
|------|------|------|
| 构建系统 | CMake | C++ 标准构建，面试通用 |
| 信令服务器 | libwebsocket (C) | 轻量、高性能 WebSocket 库 |
| WebRTC 传输 | libdatachannel | 轻量 C++ WebRTC，代码可读 |
| 视频编解码 | FFmpeg/libavcodec | H.264 编解码，行业标配 |
| 音频编解码 | Opus (libopus) | WebRTC 标准音频编解码器 |
| RTP 打包 | 自实现 | 面试核心考点，展示协议理解 |
| 视频渲染 | SDL2 | 跨平台、简单直接 |
| 音频播放 | SDL2_audio | 与渲染统一 |
| 视频采集 | V4L2 (Linux) | 直接操作摄像头，展示底层能力 |
| 音频采集 | ALSA/PulseAudio | Linux 标准音频采集 |
| 日志 | spdlog | 现代 C++ 日志库 |
| 测试 | Google Test | C++ 标准测试框架 |

## 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    CrystalRTC                        │
├──────────────┬──────────────┬───────────────────────┤
│  Signaling   │   Media      │   Transport           │
│  Server      │   Pipeline   │   Layer               │
│              │              │                       │
│  - WebSocket │  - Capture   │  - libdatachannel     │
│  - Room Mgmt │  - Encode    │  - ICE/DTLS/SRTP     │
│  - SDP Relay │  - RTP Pack  │  - DataChannel       │
│              │  - JitterBuf │                       │
│              │  - Decode    │  ┌─────────────────┐  │
│              │  - Render    │  │ NAT Traversal   │  │
│              │              │  │ - STUN Client   │  │
│              │              │  │ - TURN Relay    │  │
│              │              │  └─────────────────┘  │
├──────────────┴──────────────┴───────────────────────┤
│                   Platform Layer                     │
│              SDL2 (Render) / V4L2 (Capture)          │
└─────────────────────────────────────────────────────┘
```

## 渐进式开发路线

- **Phase 1** — P2P 音视频通话（核心 MVP）
- **Phase 2** — SFU 多人会议
- **Phase 3** — 生产级特性（加分项）

---

## Phase 1：P2P 音视频通话

### 1.1 信令服务器（Signaling Server）

**职责：** 在两个 Peer 之间转发 SDP Offer/Answer 和 ICE Candidate，不涉及媒体数据。

**协议设计（JSON over WebSocket）：**

| 消息类型 | 格式 |
|----------|------|
| join | `{type:"join", room:"xxx"}` |
| offer | `{type:"offer", sdp:"...", to:"id"}` |
| answer | `{type:"answer", sdp:"...", to:"id"}` |
| candidate | `{type:"candidate", candidate:"...", sdpMid:"0", sdpMLineIndex:0, to:"id"}` |
| leave | `{type:"leave", room:"xxx"}` |
| peer_joined | `{type:"peer_joined", peer_id:"id"}` |
| peer_left | `{type:"peer_left", peer_id:"id"}` |

**信令流程：**

```
Peer A                    Signaling Server                   Peer B
  │ join(room) ──────────►│                                  │
  │                       │◄────────────── join(room)        │
  │                       │ peer_joined ──►                  │
  │ ◄── peer_joined ──────│                                  │
  │ offer(sdp) ──────────►│ offer(sdp) ─────────────────────►│
  │ ◄── answer(sdp) ──────│◄─────────── answer(sdp)         │
  │ candidate ───────────►│ candidate ──────────────────────►│
  │ ◄── candidate ────────│◄─────────── candidate            │
  ══════════════ P2P Connection Established ═════════════════
```

**关键设计点：**
- 信令服务器只做转发，不解析 SDP 内容
- 每个 Peer 有唯一 ID，消息通过 `to` 字段定向转发
- 房间概念：同一房间内的 Peer 互相可见

### 1.2 传输层（Transport Layer）

**职责：** 封装 libdatachannel，管理 PeerConnection 生命周期。

**核心类设计：**

```
TransportManager
├── createPeerConnection(config) → PeerConnection
│   config 包含:
│   - STUN/TURN 服务器地址
│   - 媒体传输方向 (sendonly/recvonly/sendrecv)
│
├── PeerConnection
│   ├── setRemoteDescription(sdp)
│   ├── createOffer() → sdp
│   ├── createAnswer() → sdp
│   ├── addIceCandidate(candidate)
│   ├── onIceCandidate(callback)
│   ├── onTrack(callback)
│   ├── sendVideo(packet)
│   ├── sendAudio(packet)
│   └── close()
│
└── 配置
    ├── STUN: stun:stun.l.google.com:19302
    └── TURN: (可选，自建 coturn)
```

**学习重点：**
- ICE 候选收集过程（Host → SRFLX → Relay）
- DTLS 握手如何建立安全通道
- SRTP 密钥如何从 DTLS 中导出
- libdatachannel 内部如何处理这些流程

### 1.3 媒体管道（Media Pipeline）

**发送管道：**
```
Camera(V4L2) → YUV Frames → H.264 Encode(FFmpeg) → NAL Units
    → RTP Packetize(自实现) → RTP Packets → libdatachannel.send()
```

**接收管道：**
```
libdatachannel.onMessage() → RTP Packets → Jitter Buffer
    → RTP Depacketize(自实现) → NAL Units → H.264 Decode(FFmpeg)
    → YUV Frames → SDL2 Render
```

**音频管道：**
```
Send: ALSA → PCM Samples → Opus Encode → RTP Packetize → send
Recv: onMessage → RTP Depacketize → Opus Decode → SDL2 Audio Play
```

### 1.4 RTP 打包器设计

```
RtpPacketizer
├── packetizeH264(nalUnit) → vector<RtpPacket>
│   - 单 NAL 包：NAL < MTU(1200字节)，直接封装
│   - FU-A 分片：NAL > MTU，拆分为多个 FU-A 包
│   - STAP-A 聚合：多个小 NAL 打包在一起
│
├── RtpPacket 结构:
│   - Header: version(2), padding(0), extension(0)
│   - CSRC count, marker bit, payload type
│   - Sequence number (递增)
│   - Timestamp (90000Hz 时钟)
│   - SSRC (随机生成)
│   - Payload
│
└── 关键参数:
    - PT(视频): 96 (动态)
    - PT(音频): 97 (动态)
    - 视频时钟: 90000 Hz
    - 音频时钟: 48000 Hz (Opus)
```

### 1.5 Jitter Buffer 设计

```
JitterBuffer
├── insert(packet)
│   - 按 sequence number 排序
│   - 检测丢包（sequence gap）
│   - 维护滑动窗口
│
├── consume() → vector<RtpPacket>
│   - 等待足够包或超时后输出
│   - 丢弃过晚到达的包
│
├── 统计
│   - 丢包率
│   - 抖动(jitter)
│   - 平均延迟
│
└── 参数
    - 最大缓冲: 60ms
    - 最小缓冲: 20ms
    - 目标缓冲: 40ms
```

---

## Phase 2：SFU 多人会议

### 2.1 SFU 架构核心思想

```
Phase 1 (P2P):  A ←————→ B          每人发 N-1 路流

Phase 2 (SFU):  A ──►┐
                      ├──► SFU ──► 转发给 B、C、D
                 B ──►┘

                 每人只发 1 路流，SFU 负责转发
```

选择 SFU 而非 MCU，因为 SFU 是当前主流方案（Janus、mediasoup、Pion 均为 SFU），延迟低、CPU 消耗小。

### 2.2 SFU 服务器架构

```
SFUServer
├── SignalingModule          # 复用 Phase 1 信令，扩展房间消息
├── TransportModule          # 每个 Peer 一个 PeerConnection
├── Router                   # 核心：决定包转发给谁
│   ├── Publisher            # 发布者管理
│   │   ├── track_id
│   │   ├── media_type (audio/video)
│   │   └── simulcast_layers[]
│   │
│   ├── Subscriber           # 订阅者管理
│   │   ├── publisher_id
│   │   ├── selected_layer
│   │   └── peer_connection
│   │
│   └── 转发逻辑:
│       收到 Publisher 的 RTP 包
│       → 查找所有订阅该 Publisher 的 Subscriber
│       → 替换 SSRC/SeqNum（重写 RTP 头）
│       → 转发给每个 Subscriber
│
├── BandwidthEstimator       # 带宽估计
│   ├── 基于接收端反馈（RR 报告）
│   ├── 丢包率计算
│   └── 建议订阅层切换
│
└── RoomManager              # 房间生命周期管理
    ├── create/destroy room
    ├── join/leave
    ├── publish/unpublish
    └── subscribe/unsubscribe
```

### 2.3 Simulcast 设计

发送端同时发送 3 个不同分辨率的视频流，SFU 根据接收端带宽选择转发哪一层。

```
发送端:
  720p @ 1.5Mbps  ──► Simulcast Layer 0 (高)
  360p @ 600kbps  ──► Simulcast Layer 1 (中)
  180p @ 200kbps  ──► Simulcast Layer 2 (低)

SFU 转发决策:
  用户 A (好网络) → 订阅 Layer 0 (720p)
  用户 B (差网络) → 订阅 Layer 2 (180p)
  用户 C (一般)   → 订阅 Layer 1 (360p)
```

**Simulcast 编码器配置：**

```
SimulcastEncoder
├── 配置 3 路 FFmpeg 编码上下文
│   Layer 0: 1280x720, bitrate=1500kbps, fps=30
│   Layer 1: 640x360,  bitrate=600kbps,  fps=24
│   Layer 2: 320x180,  bitrate=200kbps,  fps=15
│
├── 编码流程:
│   V4L2 Capture → Scale(FFmpeg sws_scale) × 3
│   → Encode × 3 → RTP Packetize × 3 → send
│
└── RTP 区分:
    每层使用不同 SSRC
    通过 SDP 中 rid / simulcast 属性协商
```

### 2.4 码率自适应（Bandwidth Estimation）

```
BandwidthEstimator
├── 接收端统计（从 RTP 反馈获取）:
│   - 丢包率 = lost_packets / expected_packets
│   - 抖动 = |arrival_delta - send_delta| 的指数移动平均
│   - 往返时延 RTT
│
├── 带宽估计算法（简化版 GCC）:
│   if loss_rate > 10%:
│       建议降层 (Layer 0 → 1 → 2)
│   elif loss_rate < 2% and rtt_stable:
│       建议升层 (Layer 2 → 1 → 0)
│   else:
│       维持当前层
│
└── 切换策略:
    降层: 立即切换（避免卡顿）
    升层: 等待 5s 稳定后再升（避免震荡）
```

### 2.5 Phase 2 信令扩展

| 消息类型 | 格式 |
|----------|------|
| publish | `{type:"publish", tracks:[{kind,layer}]}` |
| unpublish | `{type:"unpublish", track_id:"xxx"}` |
| subscribe | `{type:"subscribe", publisher_id, layer}` |
| unsubscribe | `{type:"unsubscribe", sub_id:"xxx"}` |
| layer_change | `{type:"layer_change", sub_id, layer}` |
| stats | `{type:"stats", bitrate, loss, rtt, ...}` |

---

## Phase 3：生产级特性

### 3.1 NACK/PLI 重传机制

```
NACK (Negative Acknowledgment):
  接收端检测到 sequence gap → 发送 NACK {pid: 丢失的 seq_num}
  发送端从发送缓冲区取出对应包 → 重传

PLI (Picture Loss Indication):
  接收端长时间无法解码 → 发送 PLI
  发送端立即编码一个 IDR 帧 → 发送

实现:
├── SendBuffer              # 发送端缓冲
│   - 保存最近 500 个 RTP 包
│   - 收到 NACK 时查找并重传
│   - 超过 500 个包后淘汰
│
├── NackHandler             # 接收端 NACK 生成
│   - 检测 sequence gap
│   - 发送 NACK，最多重试 3 次
│   - 超时后触发 PLI
│
└── PliHandler              # PLI 处理
    - 解码失败累计超过阈值 → 发送 PLI
    - 收到 PLI → 立即请求编码器输出 IDR
```

### 3.2 录制回放

```
Recorder
├── 录制:
│   - RTP 包 → 解包 → NAL/PCM 原始数据
│   - 写入 MKV 容器 (FFmpeg mux)
│   - 每个参与者一个录制文件
│   - 时间戳对齐（基于 NTP 时间）
│
└── 回放:
    - 读取 MKV → 解码 → 渲染
    - 多路同步播放
```

### 3.3 统计面板

```
StatsCollector
├── 每秒采集:
│   - 发送/接收码率 (kbps)
│   - 帧率 (fps)
│   - 丢包率 (%)
│   - 抖动 (ms)
│   - RTT (ms)
│   - 编码延迟 (ms)
│   - Jitter Buffer 延迟 (ms)
│
└── 输出:
    - 控制台实时打印
    - JSON 文件导出（供后续分析）
```

---

## 错误处理

| 场景 | 处理策略 |
|------|----------|
| 网络断开 | ICE 重连 (ICE Restart) |
| 对端崩溃 | 超时检测 (5s 无数据) → 清理资源 |
| 编码失败 | 丢弃该帧，继续下一帧 |
| RTP 乱序 | Jitter Buffer 重排 |
| 丢包严重 | 触发 PLI 请求 IDR 帧 |
| 设备不可用 | 降级为纯音频模式 |

## 测试策略

- **单元测试 (Google Test):**
  - RTP 打包/解包：验证 FU-A 分片和重组
  - Jitter Buffer：模拟丢包、乱序、延迟
  - SDP 解析：验证各字段正确性

- **集成测试:**
  - 本地回环：自己发自己收，验证完整管道
  - 双端通话：两个客户端通过信令服务器连接

- **性能测试:**
  - 码率统计
  - 端到端延迟测量
  - CPU/内存占用监控

---

## 项目目录结构

```
CrystalRTC/
├── CMakeLists.txt                    # 顶层 CMake
├── third_party/                      # 第三方依赖
│   └── CMakeLists.txt                # 子项目引入
├── src/
│   ├── signaling/                    # 信令模块
│   │   ├── signaling_server.h/cpp    # WebSocket 信令服务器
│   │   └── signaling_client.h/cpp    # 信令客户端
│   ├── transport/                    # 传输层
│   │   ├── transport_manager.h/cpp   # PeerConnection 管理
│   │   └── ice_config.h              # ICE/STUN/TURN 配置
│   ├── media/                        # 媒体管道
│   │   ├── video/
│   │   │   ├── v4l2_capture.h/cpp    # 视频采集
│   │   │   ├── h264_encoder.h/cpp    # H.264 编码
│   │   │   ├── h264_decoder.h/cpp    # H.264 解码
│   │   │   └── sdl_renderer.h/cpp    # 视频渲染
│   │   ├── audio/
│   │   │   ├── alsa_capture.h/cpp    # 音频采集
│   │   │   ├── opus_encoder.h/cpp    # Opus 编码
│   │   │   ├── opus_decoder.h/cpp    # Opus 解码
│   │   │   └── sdl_audio_player.h/cpp# 音频播放
│   │   └── rtp/
│   │       ├── rtp_packet.h/cpp      # RTP 包结构
│   │       ├── rtp_packetizer.h/cpp  # RTP 打包
│   │       ├── rtp_depacketizer.h/cpp# RTP 解包
│   │       └── jitter_buffer.h/cpp   # 抖动缓冲
│   ├── room/                         # 房间管理
│   │   └── room_manager.h/cpp        # 房间/Peer 管理
│   └── utils/                        # 工具
│       ├── logger.h/cpp              # 日志封装
│       └── thread_pool.h/cpp         # 线程池
├── tests/                            # 单元测试
│   ├── rtp_packetizer_test.cpp
│   ├── rtp_depacketizer_test.cpp
│   └── jitter_buffer_test.cpp
├── configs/
│   └── default.json                  # 默认配置
└── README.md
```

## 面试知识点映射

| 模块 | 面试考点 |
|------|----------|
| 信令服务器 | SDP 各字段含义、ICE 交换流程 |
| Transport | ICE 候选类型、DTLS 握手、SRTP 密钥导出 |
| RTP 打包 | FU-A 分片逻辑、序列号/时间戳规则 |
| Jitter Buffer | 丢包检测、抖动计算、NACK 触发 |
| H.264 编解码 | NAL Unit 类型、SPS/PPS、IDR 帧 |
| Opus 编解码 | 编码模式、帧大小、比特率控制 |
| SFU Router | 发布/订阅模型、SSRC 重写、Simulcast 层选择 |
| Bandwidth Estimator | GCC 算法、丢包率计算、层切换策略 |
