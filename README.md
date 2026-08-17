# CrystalRTC — 自研 WebRTC P2P 音视频通话

从零实现的 Linux P2P 视频通话客户端：V4L2/ALSA 采集 → H.264/Opus 编码 →
自研 RTP 打包/抖动缓冲 → libdatachannel（ICE/DTLS/SRTP）传输 → 解码 →
SDL 渲染播放，并配套信令服务器完成 Offer/Answer 与 ICE 交换。

## 核心特性

- **视频链路**：V4L2 采集 YUV420P → FFmpeg libx264 编码（Baseline）→
  自研 RTP 打包（单 NAL / STAP-A / FU-A 分片，RFC 6184）→ SDL 渲染
- **音频链路**：ALSA 采集 PCM → Opus 编解码 → SDL 播放
- **P2P 传输**：ICE/STUN 候选交换、DTLS-SRTP 加密（基于 libdatachannel）
- **自研 RTP 栈**：打包/解包、JitterBuffer 乱序重排与丢包检测（回绕安全）
- **RTCP 反馈体系（全自实现，RFC 3550/4585/5761）**
  - NACK 丢包重传：JitterBuffer 间隙检测 → 状态机请求/重试/放弃 → 发送端重传缓冲补发
  - PLI 关键帧请求：解码错误/NACK 放弃触发（500ms 节流），编码器强制 IDR
  - SR/RR 质量统计：丢包率、到达间隔抖动（RFC 3550 A.8）、RTT（LSR/DLSR 法 A.6），终端周期打印
  - RTP/RTCP 单端口复用去复用（RFC 5761）

## 构建

```bash
sudo apt install libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev \
                 libopus-dev libasound2-dev libwebsockets-dev
cmake -B build && cmake --build build -j$(nproc)
```

## 运行

```bash
# 终端1：信令服务器
./build/crystal_signaling_server
# 终端2/3：两个客户端加入同一房间
./build/crystal_client room1 127.0.0.1 8765
```

弱网模拟与质量统计日志说明见 [docs/USAGE.md](docs/USAGE.md)，
学习路线见 [docs/LEARNING_GUIDE.md](docs/LEARNING_GUIDE.md)。
