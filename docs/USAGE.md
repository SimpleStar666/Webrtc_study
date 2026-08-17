# CrystalRTC 使用指南

## 🚀 快速开始（浏览器版本）

### 前置条件

你的 Windows WSL Ubuntu 需要安装：
```bash
sudo apt update && sudo apt install -y python3
```

（HTML 直接在你的 Windows 浏览器里打开！）

---

## 步骤 1：运行信令服务器

在 WSL 中执行：
```bash
cd /workspace
python3 signaling_server.py
```

你会看到：
```
==================================================
  CrystalRTC 信令服务器
==================================================
HTTP 服务器运行中: http://localhost:8764/
WebSocket 信令服务器运行中: ws://localhost:8765/
```

---

## 步骤 2：在 Windows 浏览器中打开网页

**直接在 Windows 的浏览器（Chrome/Firefox/Edge）访问：**
```
http://localhost:8764
```

打开两个标签页（分别代表两个人），你会看到视频通话界面！

---

## 界面功能说明

| 按钮/区域 | 功能 |
|----------|------|
| `连接信令` | 连接到 WebSocket 信令服务器 |
| `开始通话` | 开始本地摄像头预览 + 加入房间 |
| `结束通话` | 结束通话，关闭摄像头 |
| `本地视频` | 显示你的摄像头画面（浏览器端） |
| `远端视频` | 显示对方的画面（C++端或另一个标签页） |
| `发送字节/接收字节/往返延迟` | 实时统计数据 |

---

## 🎯 简单测试（浏览器端 - 浏览器端）

1. 打开两个浏览器标签页，都访问 `http://localhost:8764`
2. 在两个标签页上都点击 `连接信令` → `开始通话`
3. 两个标签页会自动连接，显示彼此的视频！

---

## 质量统计日志

连接建立后每 5 秒打印一行质量统计：

```
[stats] 丢包 v2.3% a0.0% | 抖动 v8.5ms a1.2ms | RTT 45ms | 重传 12 miss 2 | NACK 15 放弃 1
```

| 字段 | 含义 | 数据来源 |
|------|------|----------|
| 丢包 v/a | 对端视频/音频流的累计丢包率 | 本端 RecvSideReporter（RR 同源数据） |
| 抖动 v/a | 到达间隔平滑抖动（RFC 3550 A.8 指数平滑） | 本端 RecvSideReporter |
| RTT | 网络往返延迟（LSR/DLSR 法，RFC 3550 A.6） | 本端收到对端 RR 时计算 |
| 重传 / miss | 本端响应 NACK 补发次数 / 缓冲未命中次数 | RetransmissionBuffer |
| NACK / 放弃 | 本端发出的重传请求数 / 重试 3 次后放弃数 | NackRequester |

---

## 弱网测试（tc netem）

在**接收端**主机的出口网卡上注入丢包，验证 NACK/PLI 生效：

```bash
# 10% 丢包 + 20ms 延迟
sudo tc qdisc add dev <网卡名> root netem delay 20ms loss 10%

# 恢复
sudo tc qdisc del dev <网卡名> root
```

观察要点：
1. `[stats]` 行丢包率明显低于 netem 注入值 → NACK 重传在补包
2. 丢包率极低但出现花屏时约 0.5-1s 内恢复 → PLI 关键帧请求生效
3. `NACK 放弃` 计数增长伴随日志 `PLI sent` → 重试耗尽后 PLI 兜底
4. 对照实验：`loss 30%` 以上时 NACK 收益下降（重传也丢），主要靠 PLI 恢复

---

## 后续（C++ 端 - 浏览器端）

如果你想用 C++ 端和浏览器互操作，需要：
1. 更新 C++ 端的 SignalingClient，让它连接到 ws://localhost:8765
2. 更新 Transport 层，让 SDP 格式与浏览器兼容

如果需要，我可以继续帮你完善！

---

## 文件结构说明

| 文件/目录 | 说明 |
|----------|------|
| `web/index.html` | 浏览器端 WebRTC 客户端 |
| `signaling_server.py` | 简单的 Python 信令服务器（HTTP+WebSocket） |
| `src/` | 原有的 C++ 项目代码 |
| `docs/LEARNING_GUIDE.md` | WebRTC 学习指南 |
