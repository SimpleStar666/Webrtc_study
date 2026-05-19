# CrystalRTC 学习指南

## 如何使用本指南

本指南按**由浅入深**的顺序，带你逐模块理解 WebRTC 的核心原理。每个模块包含：
1. **概念讲解** — 这个模块解决什么问题？核心原理是什么？
2. **代码走读** — 对应代码在哪里，关键逻辑在哪些行
3. **动手练习** — 修改代码观察效果，加深理解
4. **面试高频题** — 面试中关于这个模块的常见问题

**建议学习顺序：** Module 1 → 2 → 3 → 4 → 5 → 6 → 7

---

## Module 1: RTP 协议 — WebRTC 传输的基石

### 概念讲解

RTP (Real-time Transport Protocol) 是 WebRTC 传输音视频数据的协议。所有音视频数据都先封装成 RTP 包，再通过 UDP 发送。

**为什么不用 TCP？** TCP 的重传机制会导致延迟累积。视频通话中，一个迟到的包比一个丢失的包更糟糕——迟到意味着卡顿，丢失最多一帧花屏。

**RTP 包结构（12字节固定头）：**
```
字节0:  V(2bit)=2 | P(1bit) | X(1bit) | CC(4bit)
字节1:  M(1bit)   | PT(7bit)
字节2-3: Sequence Number (16bit)
字节4-7: Timestamp (32bit)
字节8-11: SSRC (32bit)
字节12+: Payload
```

**各字段含义：**
- **V (Version)**: 固定为 2，RTP 协议版本
- **M (Marker)**: 视频中标记一帧的最后一个包；音频中标记一帧的开始
- **PT (Payload Type)**: 96-127 是动态分配的。我们用 96=H.264, 97=Opus
- **Sequence Number**: 每发一个包 +1，接收端用来检测丢包和排序
- **Timestamp**: 视频用 90000Hz 时钟，音频用 48000Hz 时钟
- **SSRC**: 同一个 RTP 流的唯一标识（随机生成）

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/media/rtp/rtp_packet.h` | RtpPacket 类定义，所有字段 |
| `src/media/rtp/rtp_packet.cpp:parse()` | 从字节流解析 RTP 头（第 17-55 行） |
| `src/media/rtp/rtp_packet.cpp:serialize()` | 将 RTP 头序列化为字节流（第 57-83 行） |

**重点理解 `parse()` 中的字节拆分：**
```cpp
// 字节0的高2位是版本号
if ((byte0 >> 6) != 2) return false;

// 字节1的最高位是 Marker
marker_ = (byte1 & 0x80) != 0;

// 字节1的低7位是 Payload Type
payloadType_ = byte1 & 0x7F;

// 字节2-3 是 Sequence Number（大端序）
sequenceNumber_ = (data[2] << 8) | data[3];
```

### 动手练习

**练习 1.1：** 运行 `./crystal_demo`，观察 Demo 1 输出的字节流。手动验证：
- `80` = `1000 0000` → V=2, P=0, X=0, CC=0
- `E0` = `1110 0000` → M=1, PT=96(0x60)

**练习 1.2：** 修改 `main_demo.cpp`，把 SSRC 改成 `0xCAFEBABE`，重新编译运行，观察字节流的变化。

**练习 1.3：** 在 `rtp_packet.cpp` 的 `parse()` 中，如果收到的版本号不是 2 会怎样？为什么这个检查很重要？

### 面试高频题

1. **RTP 和 UDP 的关系是什么？** RTP 运行在 UDP 之上，RTP 定义了包格式和序号机制，UDP 提供传输服务。
2. **为什么视频时钟是 90000Hz？** 因为 90000 是常见帧率的整数倍（30fps → 3000 ticks/frame, 24fps → 3750 ticks/frame）。
3. **Sequence Number 和 Timestamp 的区别？** SeqNum 每包+1，用于排序和丢包检测；Timestamp 反映采样时间，用于同步和抖动计算。一个视频帧可能对应多个 RTP 包（它们 Timestamp 相同但 SeqNum 不同）。

---

## Module 2: H.264 FU-A 分片 — 面试必考

### 概念讲解

一个 H.264 IDR 帧可能有几十 KB，但网络 MTU 只有 1500 字节。FU-A (Fragmentation Unit Type A) 就是把大 NAL 拆成小片的机制。

**三种 RTP/H264 打包模式：**

| 模式 | 条件 | 说明 |
|------|------|------|
| 单 NAL 包 | NAL < MTU | 直接把 NAL 放进 RTP 负载 |
| FU-A 分片 | NAL > MTU | 拆成多个 FU-A 包 |
| STAP-A 聚合 | 多个小 NAL | 打包到一个 RTP 包 |

**FU-A 包格式（2字节额外头）：**
```
FU Indicator (1字节):
  forbidden_zero_bit(1) = 0
  nal_ref_idc(2)        = 原始NAL的nal_ref_idc
  type(5)               = 28 (FU-A的类型号)

FU Header (1字节):
  start(1)    = 1表示这是第一个分片
  end(1)      = 1表示这是最后一个分片
  reserved(1) = 0
  type(5)     = 原始NAL的type
```

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/media/rtp/rtp_packetizer.cpp:packetizeH264()` | FU-A 分片逻辑 |
| `src/media/rtp/rtp_depacketizer.cpp:depacketizeH264()` | FU-A 重组逻辑 |

**重点理解 `packetizeH264()` 中的分片逻辑：**
```cpp
// 提取原始 NAL 的 ref_idc 和 type
uint8_t nalRefIdc = (nalUnit[0] & 0x60) >> 5;  // 第0字节的6-5位
uint8_t nalType = nalUnit[0] & 0x1F;             // 第0字节的4-0位

// 跳过原始 NAL 头字节，从第1字节开始分片
const uint8_t* nalData = nalUnit.data() + 1;
size_t nalDataLen = nalUnit.size() - 1;

// 每个分片最大负载 = MTU - 2(FU头)
size_t maxFragLen = maxPacketSize_ - 2;

// 构造 FU Indicator: 保持原始的 nal_ref_idc，type 改为 28
uint8_t fuIndicator = (nalRefIdc << 5) | 28;

// 构造 FU Header: start/end 位 + 原始 NAL type
uint8_t fuHeader = (nalType & 0x1F);
if (isFirst) fuHeader |= 0x80;  // 设置 start 位
if (isLast) fuHeader |= 0x40;   // 设置 end 位
```

### 动手练习

**练习 2.1：** 运行 `./crystal_demo`，观察 Demo 2 的输出。一个 3000 字节的 NAL 被拆成了几个 RTP 包？每个包的 FU Header 的 start/end 位是什么？

**练习 2.2：** 修改 `main_demo.cpp`，把 `largeNal` 大小改为 5000 字节，观察分成了几个包。

**练习 2.3（进阶）：** 在 `rtp_packetizer.cpp` 中，如果把 `maxPacketSize_` 从 1200 改为 500，会发生什么？为什么？

### 面试高频题

1. **FU-A 的 start 包和 end 包的 marker 位分别是什么？** 只有 end 包的 marker=1（表示帧结束），start 和 middle 包 marker=0。
2. **接收端如何知道一个 FU-A 序列结束了？** 收到 FU Header 的 end=1 的包。
3. **为什么 FU-A 要保留原始 NAL 的 nal_ref_idc？** nal_ref_idc 表示这个 NAL 的重要性（3=最高，0=可丢弃），接收端需要这个信息来决定丢包时的处理策略。
4. **如果 FU-A 的中间某个包丢了，接收端怎么处理？** 丢弃整个 FU-A 序列，等待下一个 IDR 帧。

---

## Module 3: Jitter Buffer — 对抗网络抖动

### 概念讲解

网络传输中，RTP 包可能乱序到达、延迟到达、或丢失。Jitter Buffer 的作用是：
1. **缓存** — 等待一小段时间再输出，给迟到的包一个机会
2. **排序** — 按 sequence number 重排
3. **丢包检测** — 发现 sequence gap 就知道丢了包

**抖动 (Jitter) 计算（RFC 3550）：**
```
J = J + (|D(i-1,i)| - J) / 16
其中 D(i-1,i) = (Ri - Ri-1) - (Si - Si-1)
R = 到达时间, S = RTP时间戳
```

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/media/rtp/jitter_buffer.cpp:insert()` | 包插入和丢包检测 |
| `src/media/rtp/jitter_buffer.cpp:consume()` | 有序输出 |

**重点理解 `insert()` 中的丢包检测：**
```cpp
int16_t diff = static_cast<int16_t>(seq - expectedSeq_);
// diff == 0: 正常，期望的包到了
// diff > 0:  跳过了一些包，diff-1 就是丢包数
// diff < 0:  迟到的包或重复包
```

**注意 `int16_t` 的妙用：** Sequence Number 是 uint16_t，会回绕（65535→0）。用 `int16_t` 做差值，可以正确处理回绕：`(uint16_t)5 - (uint16_t)65530 = (int16_t)11`。

### 动手练习

**练习 3.1：** 运行 `./crystal_demo`，观察 Demo 3 中三种场景的输出。

**练习 3.2：** 修改 `main_demo.cpp`，模拟 Sequence Number 回绕：先发 seq=65534, 65535, 再发 seq=0, 1。验证 Jitter Buffer 能否正确处理。

**练习 3.3（进阶）：** 当前 Jitter Buffer 的 `consume()` 是立即输出的。真实 WebRTC 中，consume 应该等一段时间再输出（比如 40ms）。思考：如何实现一个基于时间的 consume？

### 面试高频题

1. **Jitter Buffer 太大和太小分别有什么问题？** 太大→延迟高；太小→乱序包来不及到达就被跳过，花屏。
2. **如何自适应调整 Jitter Buffer 大小？** 根据网络抖动动态调整：抖动大→增大缓冲，抖动小→减小缓冲。
3. **如何检测 Sequence Number 回绕？** 用 `int16_t` 做差值，如果差值 > 32768 说明是回绕而不是乱序。

---

## Module 4: H.264 编解码 — 视频压缩核心

### 概念讲解

**H.264 NAL Unit 类型（面试必记）：**

| Type | 名称 | 说明 |
|------|------|------|
| 1 | 非 IDR 切片 | P帧或B帧 |
| 5 | IDR 切片 | 关键帧，可以独立解码 |
| 6 | SEI | 补充信息 |
| 7 | SPS | 序列参数集（分辨率、帧率等） |
| 8 | PPS | 图像参数集（编码参数） |

**解码器启动流程：** 必须先收到 SPS+PPS，才能解码 IDR 帧。如果只收到 P 帧，解码器无法工作。

**NAL Unit 首字节解析：**
```
forbidden_zero_bit(1) = 0 (必须为0，为1表示语法错误)
nal_ref_idc(2)        = 3(IDR/SPS/PPS), 2(P帧), 0(非参考帧)
type(5)               = 见上表
```

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/media/video/h264_encoder.cpp:init()` | 编码器参数配置 |
| `src/media/video/h264_encoder.cpp:encode()` | 编码一帧 YUV |
| `src/media/video/h264_encoder.cpp:processPacket()` | 从 AVPacket 提取 NAL |
| `src/media/video/h264_decoder.cpp:decode()` | 解码 NAL 为 YUV |

**重点理解编码器参数：**
```cpp
codecCtx_->gop_size = 30;          // 每30帧一个IDR（关键帧）
codecCtx_->max_b_frames = 0;       // 不用B帧（实时通话延迟要求）
av_opt_set(..., "preset", "ultrafast");  // 编码速度优先
av_opt_set(..., "tune", "zerolatency");  // 零延迟模式
av_opt_set(..., "profile", "baseline");  // 基线配置（兼容性最好）
```

### 动手练习

**练习 4.1：** 运行 `./crystal_demo`，观察 Demo 4 中编码输出了什么类型的 NAL？为什么第一个就是 IDR(type=5)？

**练习 4.2：** 修改 `h264_encoder.cpp` 中的 `gop_size` 从 30 改为 5，重新编译运行 demo。观察编码输出的 NAL 类型变化。

**练习 4.3（进阶）：** 修改 `h264_encoder.cpp`，在 `processPacket` 中打印每个 NAL 的类型。连续编码多帧，观察 SPS/PPS/IDR/P 的出现规律。

### 面试高频题

1. **为什么实时通话不用 B 帧？** B 帧需要后续帧才能解码，增加延迟。实时通话优先低延迟。
2. **SPS 和 PPS 的作用？** SPS 包含分辨率、帧率等序列级参数；PPS 包含熵编码模式、量化参数等图像级参数。没有它们，解码器无法初始化。
3. **为什么用 baseline profile？** 兼容性最好，不支持 B 帧和 CABAC，但延迟最低。

---

## Module 5: Opus 音频编解码

### 概念讲解

Opus 是 WebRTC 的标准音频编解码器，特点：
- **码率范围：** 6 kbps ~ 510 kbps
- **采样率：** 48kHz
- **帧大小：** 2.5ms ~ 60ms（常用 20ms = 960 samples）
- **模式：** SILK（语音） / CELT（音乐） / 混合（自动切换）

**关键参数：**
```cpp
OPUS_APPLICATION_VOIP  // 语音优化
OPUS_APPLICATION_AUDIO // 音乐优化
OPUS_SET_DTX(1)        // 静音时不发送数据（节省带宽）
OPUS_SET_COMPLEXITY(5) // 编码复杂度 0-10，5是平衡点
```

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/media/audio/opus_encoder.cpp:init()` | 编码器初始化和参数设置 |
| `src/media/audio/opus_encoder.cpp:encode()` | PCM → Opus 编码 |
| `src/media/audio/opus_decoder.cpp:decode()` | Opus → PCM 解码 |

### 动手练习

**练习 5.1：** 修改 `opus_encoder.cpp` 中的 `OPUS_SET_BITRATE`，从 64kbps 改为 16kbps。观察编码后 Opus 帧大小的变化。

**练习 5.2：** 修改 `OPUS_APPLICATION_VOIP` 为 `OPUS_APPLICATION_AUDIO`，思考什么场景下应该用哪个。

### 面试高频题

1. **Opus 为什么比 AAC 更适合实时通话？** Opus 有 DTX（静音检测）、更低延迟、码率自适应。
2. **DTX 是什么？** Discontinuous Transmission，静音时不发数据，节省带宽。
3. **Opus 的帧大小 20ms 意味着什么？** 每 20ms 编码一次，产生一个 Opus 帧。960 samples × 2字节 = 1920 字节 PCM 输入。

---

## Module 6: 信令服务器 — WebRTC 的"红娘"

### 概念讲解

WebRTC 本身**不定义信令协议**。两个 Peer 需要通过其他通道交换：
1. **SDP (Session Description Protocol)** — 描述媒体能力（编码格式、分辨率等）
2. **ICE Candidate** — 网络地址信息（IP+端口）

信令服务器就是这个"其他通道"——一个简单的消息转发服务器。

**信令流程（面试必画）：**
```
Peer A                    Server                   Peer B
  │ join(room) ──────────►│                         │
  │                       │◄─── join(room)           │
  │ ◄── peer_joined ──────│─── peer_joined ────────►│
  │ offer(sdp) ──────────►│─── offer(sdp) ─────────►│
  │ ◄── answer(sdp) ──────│◄── answer(sdp)          │
  │ candidate ───────────►│─── candidate ──────────►│
  │ ◄── candidate ────────│◄── candidate             │
  ══════════════ P2P 直连建立 ═══════════════════════
```

**关键理解：** 信令服务器只转发消息，不处理媒体数据。一旦 P2P 连接建立，音视频数据直接在两个 Peer 之间传输，不经过服务器。

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/signaling/signaling_server.cpp:handleMessage()` | 消息路由逻辑 |
| `src/signaling/signaling_server.cpp:sendToPeer()` | 定向发送 |
| `src/signaling/signaling_server.cpp:broadcastToRoom()` | 房间广播 |

### 面试高频题

1. **为什么 WebRTC 不定义信令协议？** 不同场景需求不同（SIP/Jingle/自定义），保持灵活性。
2. **SDP Offer/Answer 模型是什么？** 发起方发 Offer，应答方回 Answer。Offer 包含所有媒体能力，Answer 选择其中支持的。
3. **ICE Candidate 有哪几种类型？** Host（本机地址）、SRFLX（STUN 反射地址）、Relay（TURN 中继地址）。

---

## Module 7: 传输层 — ICE/DTLS/SRTP

### 概念讲解

WebRTC 建立连接的核心流程：

```
1. ICE (Interactive Connectivity Establishment)
   → 找到两个 Peer 之间可用的网络路径
   → 尝试顺序: Host → SRFLX(STUN) → Relay(TURN)

2. DTLS (Datagram TLS)
   → 在 UDP 上建立加密通道（类似 TLS 但基于 UDP）
   → 握手完成后，导出密钥给 SRTP 使用

3. SRTP (Secure RTP)
   → 用 DTLS 导出的密钥加密 RTP 包
   → 每个 RTP 包都加密，防止窃听
```

**NAT 穿透问题：**
- **STUN** — 告诉你你的公网 IP 是什么（"你在别人眼里是谁"）
- **TURN** — 当直连不通时，通过中继服务器转发（"帮你转交"）

### 代码走读

| 文件 | 关键内容 |
|------|----------|
| `src/transport/transport_manager.cpp:PeerConnection()` | ICE 配置和回调注册 |
| `src/transport/transport_manager.cpp:createOffer()` | 创建 Offer |
| `src/transport/transport_manager.cpp:setRemoteDescription()` | 设置远端 SDP |

**重点理解 libdatachannel 封装了什么：**
```cpp
// ICE 配置
rtc::Configuration rtcConfig;
rtcConfig.iceServers.emplace_back("stun:stun.l.google.com:19302");

// ICE 候选回调
pc_->onLocalCandidate([](const rtc::Candidate& cand) {
    // 每发现一个候选地址，就通过信令告诉对方
});

// 连接状态回调
pc_->onStateChange([](rtc::PeerConnection::State state) {
    // Connected = ICE+DTLS 都完成了，可以发数据了
});
```

### 面试高频题

1. **ICE 的全称和作用？** Interactive Connectivity Establishment，在 NAT 环境下找到可用的网络路径。
2. **什么时候需要 TURN？** 双方都在对称 NAT 后面，STUN 无法穿透，必须用 TURN 中继。
3. **DTLS 和 TLS 的区别？** DTLS 基于 UDP，TLS 基于 TCP。DTLS 需要处理丢包和乱序。
4. **SRTP 密钥从哪里来？** 从 DTLS 握手结果中导出（DTLS-SRTP），不需要额外协商。

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

**任务：** 在每个回调函数中加一行日志，运行时观察数据流的完整路径。

### 练习 B：模拟丢包

修改 `main_demo.cpp` 的 Demo 4，在发送端和接收端之间随机丢弃 10% 的 RTP 包：
```cpp
// 在发送循环中添加
if (rand() % 10 == 0) continue; // 10% 概率丢包
```
观察：解码是否还能工作？画面会怎样？

### 练习 C：实现 STAP-A

当前 `RtpPacketizer` 只实现了单 NAL 和 FU-A。尝试实现 STAP-A 模式：将 SPS+PPS+IDR 头打包到一个 RTP 包中。

提示：STAP-A 格式为 `type=24` + `NAL1大小(2字节)` + `NAL1数据` + `NAL2大小(2字节)` + `NAL2数据` ...

---

## 推荐阅读

| 资源 | 内容 |
|------|------|
| RFC 3550 | RTP 协议规范（必读） |
| RFC 6184 | RTP Payload Format for H.264 Video（FU-A 定义在这里） |
| RFC 5764 | DTLS-SRTP 密钥导出机制 |
| RFC 8445 | ICE 协议规范 |
| libdatachannel 源码 | 理解 WebRTC 传输层的实际实现 |
