# Phase C：GCC/Trendline 带宽估计 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现完整 GCC（Google Congestion Control）发送端带宽估计闭环：TWCC 扩展头 → TransportFeedback RTCP 包 → Trendline 斜率估计 → 过载检测状态机 + AIMD 速率控制 → H.264 运行时调码率。

**Architecture:** 自底向上五组件——①`RtpPacket` 扩展 one-byte header extension（0xBEDE + ID=1 传输序号）；②`TransportFeedback`（PT=205/FMT=15，Google 格式）编解码；③`TwccRecorder` 接收端记录 (seq, 到达时刻) 每 100ms 打包反馈；④`TrendlineEstimator` 对 (到达时刻, 累积延迟) 做最小二乘线性回归；⑤`GccController` 双通道融合（丢包率通道 + 延迟梯度通道）+ AIMD。发送端收到 feedback 后结合本地发包时刻还原延迟样本，输出目标码率应用到编码器。

**Tech Stack:** 纯 C++ 标准库（无新依赖）、googletest。

**设计文档:** `docs/superpowers/specs/2026-09-09-crystalrtc-v2-engineering-upgrade-design.md` 第 6 节（原 Phase D，应用户要求提前执行）

**顺序调整说明:** 设计文档中本阶段原为 Phase D（延迟与线程是原 Phase C）。因 GCC 是客户端/终端 SDK 方向的核心竞争力且价值更高，经确认提前实施；延迟与线程（JitterBuffer 时间驱动释放、SPSC 音频缓冲、编码线程解耦）顺延为后续阶段。

```
发送端                                        接收端
每个RTP包打TWCC扩展头(0xBEDE,ID=1,2字节序号) ──→ TwccRecorder 记录(seq,到达时刻)
                                             每100ms 打RTCP TransportFeedback ──→
Trendline滤波(OWD一阶差分线性回归,20样本窗) ←── 解码feedback还原样本
        ↓
过载检测状态机(NORMAL/OVERUSE)
丢包率通道(>10%强降/<2%交棒趋势通道)     ─→ AIMD速率控制器(×0.85降/递增)
                                         ↓      目标码率(钳[100,4000]kbps)
                          H264Encoder::setBitrate() 运行时调码率
```

**关键设计决策（简化与生产差异，写指南时展开）：**

| 决策 | 简化 | libwebrtc 生产做法 |
|------|------|-------------------|
| packet chunk | 只用 StatusVectorChunk(2-bit symbol) | RunLength + StatusVector 混合压缩 |
| receive delta | 全用 2 字节有符号(250us) | small(1B)/big(2B) 按需选择 |
| 过载阈值 | 固定斜率阈值 | 自适应阈值（历史斜率的滑动分位数） |
| 探测 | 无 | ProbeController（探测可用带宽） |
| pacer | 无（码率直接作用编码器） | PacedSender 平滑发包 |

---

### Task 1: TWCC 扩展头（RtpPacket 支持 one-byte extension）

**Files:**
- Modify: `src/media/rtp/rtp_packet.h`（新增成员 + 访问器）
- Modify: `src/media/rtp/rtp_packet.cpp`（parse/serialize 支持扩展头）
- Test: `tests/twcc_header_test.cpp`（新建）

- [ ] **Step 1: 写失败测试**

创建 `tests/twcc_header_test.cpp`：

```cpp
// ============================================================================
// twcc_header_test.cpp — TWCC 扩展头单元测试（工程化升级 v2 新增）
// ============================================================================
// 验证 RFC 8285 one-byte header extension 格式的往返一致性：
//   setTwccSeq → serialize → parse → 读回 twccSeq
// 以及向后兼容：未设置扩展的包仍是纯 12 字节头。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtp/rtp_packet.h"

// 设置了 TWCC 序号 → 序列化 → 解析 → 应读回同一序号 + 扩展头存在
TEST(TwccHeader, RoundTrip) {
    crystal::RtpPacket p;
    p.setPayloadType(96);
    p.setSequenceNumber(100);
    p.setTimestamp(90000);
    p.setSsrc(0x12345678);
    p.setTwccSeq(32167);
    p.setPayload({0x01, 0x02, 0x03});

    auto bytes = p.serialize();
    // 头部 = 12 固定 + 4 扩展头 + 4 TWCC 块 = 20 字节
    ASSERT_EQ(bytes.size(), 20u + 3u);

    // 第二字节 X 位应置位（PT=96 → 0x60 | 0x10 = 0x70）
    EXPECT_EQ(bytes[1], 0x70);

    // 扩展头：0xBE 0xDE + length=1（以 32bit 字计，4 字节数据/4-1=1）
    EXPECT_EQ(bytes[12], 0xBE);
    EXPECT_EQ(bytes[13], 0xDE);
    EXPECT_EQ(bytes[14], 0x00);
    EXPECT_EQ(bytes[15], 0x01);
    // TWCC 块：ID=1 | L=1（数据 2 字节，L=2-1）| 序号大端
    EXPECT_EQ(bytes[16], 0x11);
    EXPECT_EQ(bytes[17], 32167 >> 8);
    EXPECT_EQ(bytes[18], 32167 & 0xFF);

    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_TRUE(q.extension());
    EXPECT_EQ(q.twccSeq(), 32167);
    EXPECT_EQ(q.sequenceNumber(), 100);
    EXPECT_EQ(q.payload().size(), 3u);
}

// 未设置 TWCC → 保持纯 12 字节头（向后兼容，音频流不打 TWCC）
TEST(TwccHeader, NoExtensionStillWorks) {
    crystal::RtpPacket p;
    p.setPayloadType(97);
    p.setSequenceNumber(7);
    p.setPayload({0xAA});
    auto bytes = p.serialize();
    ASSERT_EQ(bytes.size(), 13u);
    EXPECT_EQ(bytes[1], 0x80 | 97);  // X=0

    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_FALSE(q.extension());
}

// 序号回绕：65535 → 0
TEST(TwccHeader, SeqWraparound) {
    crystal::RtpPacket p;
    p.setTwccSeq(65535);
    auto bytes = p.serialize();
    crystal::RtpPacket q;
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_EQ(q.twccSeq(), 65535);
    p.setTwccSeq(0);
    bytes = p.serialize();
    ASSERT_TRUE(q.parse(bytes.data(), bytes.size()));
    EXPECT_EQ(q.twccSeq(), 0);
}
```

- [ ] **Step 2: tests/CMakeLists.txt 加目标，跑失败**

`tests/CMakeLists.txt` 修改 `crystal_rtp_tests` 目标（把 twcc_header_test.cpp 加入其源列表——它测的是 RTP 层）：

```cmake
add_executable(crystal_rtp_tests
    rtp_packet_test.cpp
    rtp_packetizer_test.cpp
    rtp_depacketizer_test.cpp
    jitter_buffer_test.cpp
    twcc_header_test.cpp
)
```

Run: `cd /workspace/build && cmake -S .. -B . > /dev/null && cmake --build . --target crystal_rtp_tests -j4 2>&1 | grep -E "error" | head -3`
Expected: FAIL —— `setTwccSeq`/`twccSeq` 不是 RtpPacket 的成员

- [ ] **Step 3: 实现**

`rtp_packet.h` 头部注释的【当前实现的简化】段落更新为支持扩展；在 `setPayload(const uint8_t*, size_t)` 声明后追加：

```cpp
    // ========================================================================
    // TWCC 扩展头（工程化升级 v2 新增）
    // ========================================================================

    // 设置传输层扩展序号（0~65535，每发一个 RTP 包 +1）
    // 设置后 serialize() 会置 X 位并追加 one-byte extension 块（RFC 8285）：
    //   扩展头 0xBEDE + length | ID=1 | L=1 | 序号(2B)
    // GCC 用它把"同一时刻发出的多个包"关联起来做带宽估计
    void setTwccSeq(uint16_t seq);

    // 读取 TWCC 扩展序号；未设置（无扩展头或无 ID=1 块）返回 false
    bool getTwccSeq(uint16_t& seq) const;

    // ---- 旧接口兼容：getter 风格（测试与接线用）----
    // 是否携带 TWCC 扩展（X=1 且存在 ID=1 块）
    bool hasTwcc() const { return hasTwcc_; }
    // 直接读 TWCC 序号（未设置时返回 0；配合 hasTwcc() 使用）
    uint16_t twccSeq() const { return twccSeq_; }
```

私有成员追加（`payload_` 之后）：

```cpp
    // TWCC 扩展头状态（v2 新增）
    bool hasTwcc_ = false;        // 是否携带 TWCC 扩展块
    uint16_t twccSeq_ = 0;        // 传输层扩展序号（GCC 带宽估计用）
```

`extension()` 访问器改为返回真实状态：

```cpp
    // 获取扩展标志
    // 返回值: X 位是否置位（v2 起真实反映解析结果/设置状态）
    bool extension() const;
```

`rtp_packet.cpp`：

1. `parse()`：固定头 12 字节解析后，若 X=1，读 4 字节扩展头（0xBEDE + length），逐块遍历 one-byte extension：块首字节高 4 位是 ID、低 4 位是 len（数据长度-1），ID=1 且 len=1 时读 2 字节序号。跳过 length*4 字节扩展区后才是 payload。解析不到 ID=1 块则 hasTwcc_ 保持 false（payload 正常提取）。
2. `serialize()`：若 hasTwcc_，Byte0 的 X 位置 1，固定头后追加 `{0xBE, 0xDE, 0x00, 0x01, 0x11, seq>>8, seq&0xFF, 0x00}`——注意 TWCC 块是 4 字节（ID/L 1 字节 + 序号 2 字节 + 补齐 1 字节？不：one-byte 块 ID+len+data = 1+2=3 字节，补 1 字节零对齐到 4）。**修正：`{0xBE, 0xDE, 0x00, 0x01}` + `{0x11, seqHi, seqLo, 0x00}`，扩展区共 8 字节=2 个 32bit 字，length 字段 = 8/4 - 1 = 1。**
3. `headerSize()`：`hasTwcc_` 时返回 20，否则 12。
4. `extension()`：返回 `hasTwcc_`（简化：本项目只有 TWCC 一种扩展，X 位与 hasTwcc_ 一致）。

- [ ] **Step 4: 跑通过**

Run: `cmake --build . --target crystal_rtp_tests -j4 && ./tests/crystal_rtp_tests`
Expected: 全部 PASSED（原有 RTP 测试 + 3 个新测试）

- [ ] **Step 5: 提交**

```bash
git add src/media/rtp/rtp_packet.h src/media/rtp/rtp_packet.cpp tests/twcc_header_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtp): TWCC one-byte 扩展头——serialize/parse 双向支持"
```

---

### Task 2: TransportFeedback RTCP 包编解码

**Files:**
- Create: `src/media/rtcp/transport_feedback.h`
- Create: `src/media/rtcp/transport_feedback.cpp`
- Modify: `src/media/rtcp/rtcp_packet.h`（RtcpKind + 结构持有）
- Modify: `src/media/rtcp/rtcp_packet.cpp`（复合包解析 FMT=15 分支）
- Modify: `src/CMakeLists.txt`（crystal_media_rtcp 源列表）
- Test: `tests/transport_feedback_test.cpp`（新建）

- [ ] **Step 1: 写失败测试**

创建 `tests/transport_feedback_test.cpp`：

```cpp
// ============================================================================
// transport_feedback_test.cpp — TWCC TransportFeedback 编解码测试（v2 新增）
// ============================================================================
// Google TWCC feedback 格式（draft-holmer-rmcat-feedback）：
//   头部 20 字节（含 RTCP 公共头）+ N*2B StatusVectorChunk + M*2B delta
// 简化策略：全用 2-bit symbol 的 StatusVectorChunk + 2 字节有符号 delta(250us)
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/transport_feedback.h"
#include "media/rtcp/rtcp_packet.h"

// 编码 → 解码 round-trip：3 个包（1 收到、1 丢失、1 收到）
TEST(TransportFeedback, RoundTrip) {
    crystal::TwccFeedback fb;
    fb.senderSsrc = 0x1111;
    fb.mediaSsrc = 0x2222;
    // 样本：seq 100 于 10ms 到达，101 丢失，102 于 35ms 到达
    fb.baseSeq = 100;
    fb.refTimeMs = 10;  // 基准时刻（1/64ms 网格）
    fb.received = {{100, 10.0}, {102, 35.0}};  // (seq, arrivalMs) 无序容忍
    fb.lost = {101};

    auto bytes = crystal::appendTransportFeedback(fb);

    // RTCP 公共头检查：V=2, FMT=15, PT=205
    EXPECT_EQ(bytes[0] >> 6, 2);
    EXPECT_EQ(bytes[0] & 0x1F, 15);
    EXPECT_EQ(bytes[1], 205);

    // 解码（走复合包解析路径）
    crystal::TwccFeedback decoded;
    int hit = 0;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) {
                decoded = p.twcc;
                hit++;
            }
        });
    ASSERT_EQ(hit, 1);
    EXPECT_EQ(decoded.senderSsrc, 0x1111u);
    EXPECT_EQ(decoded.mediaSsrc, 0x2222u);
    EXPECT_EQ(decoded.baseSeq, 100u);

    // 到达时刻还原：refTime 基准 + delta 累加（250us 网格有量化误差，±1ms 内）
    ASSERT_EQ(decoded.received.size(), 2u);
    EXPECT_NEAR(decoded.received[0].arrivalMs, 10.0, 1.0);
    EXPECT_NEAR(decoded.received[1].arrivalMs, 35.0, 1.0);
    EXPECT_EQ(decoded.lost, (std::vector<uint16_t>{101}));
}

// 序号回绕 round-trip：base=65534, 丢 0
TEST(TransportFeedback, Wraparound) {
    crystal::TwccFeedback fb;
    fb.baseSeq = 65534;
    fb.refTimeMs = 5;
    fb.received = {{65534, 5.0}, {65535, 8.0}};
    fb.lost = {0};  // 65534, 65535, 0 三个包中 0 丢失
    auto bytes = crystal::appendTransportFeedback(fb);

    crystal::TwccFeedback decoded;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) decoded = p.twcc;
        });
    EXPECT_EQ(decoded.lost, (std::vector<uint16_t>{0}));
    ASSERT_EQ(decoded.received.size(), 2u);
}

// 长窗口：30 个包（超过 4 个 chunk × 7 symbol，验证多 chunk 拼接）
TEST(TransportFeedback, MultiChunk) {
    crystal::TwccFeedback fb;
    fb.baseSeq = 1000;
    fb.refTimeMs = 0;
    for (uint32_t i = 0; i < 30; i += 2) {
        fb.received.push_back({1000 + i, i * 1.0});   // 偶数包到达
        fb.lost.push_back(static_cast<uint16_t>(1000 + i + 1));  // 奇数包丢失
    }
    auto bytes = crystal::appendTransportFeedback(fb);
    crystal::TwccFeedback decoded;
    crystal::parseRtcpCompound(bytes.data(), bytes.size(),
        [&](const crystal::RtcpPacket& p) {
            if (p.kind == crystal::RtcpKind::TransportFeedback) decoded = p.twcc;
        });
    EXPECT_EQ(decoded.received.size(), 15u);
    EXPECT_EQ(decoded.lost.size(), 15u);
}
```

- [ ] **Step 2: CMake 加测试目标 + 跑失败**

`tests/CMakeLists.txt`：把 `transport_feedback_test.cpp` 加入 `crystal_rtcp_tests` 源列表。

Run: `cmake --build . --target crystal_rtcp_tests -j4 2>&1 | grep -E "error" | head -3`
Expected: FAIL —— `transport_feedback.h` 不存在

- [ ] **Step 3: 实现**

创建 `src/media/rtcp/transport_feedback.h`：

```cpp
// ============================================================================
// transport_feedback.h - TWCC TransportFeedback 反馈包（v2 新增）
// ============================================================================
// 【在 GCC 中的角色】
// 接收端把"每个 RTP 包的到达时刻"打包回传给发送端，发送端据此计算
// 单向延迟的变化趋势（不需要收发时钟同步——只用差分）。
//
//【报文格式（Google draft-holmer-rmcat-feedback，libwebrtc 在用）】
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |V=2|P| FMT=15  |   PT=205     |          length               |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                     SSRC of packet sender                     |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |                     SSRC of media source                      |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |      base sequence number     | packet status count  |  B=0  |
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
// |            reference time (24bit, 1/64ms) |fb pkt cnt| chunks..
// +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
//【本实现的简化】
// chunk 只用 StatusVectorChunk（2-bit symbol，7 symbol/chunk）：
//   00 = 未收到，01 = 已收到（跟随 2 字节 delta）
// delta 全用 2 字节有符号（250us 单位）。生产实现还有 RunLengthChunk
// 压缩连续相同状态、small delta 1 字节模式——见指南"生产差异"。
#pragma once

#include <cstdint>
#include <vector>

namespace crystal {

constexpr uint8_t RTCP_FMT_TWCC = 15;  // PT=205 的 TWCC FMT

// 到达样本：seq 与到达时刻（毫秒，接收端本地时钟）
struct ArrivalSample {
    uint16_t seq = 0;
    double arrivalMs = 0;
};

// TWCC 反馈包（编码前/解码后的结构化形态）
struct TwccFeedback {
    uint32_t senderSsrc = 0;   // 发 feedback 一方（接收端）
    uint32_t mediaSsrc = 0;    // 被观测的媒体流（发送端）
    uint16_t baseSeq = 0;     // 本窗口第一个包的 seq
    double refTimeMs = 0;     // 基准到达时刻（编码为 1/64ms 网格）
    std::vector<ArrivalSample> received;  // 收到的包（按 seq 升序）
    std::vector<uint16_t> lost;           // 丢失的包
};

// 编码为完整 RTCP 子包字节流（含公共头，可直接并入复合包）
std::vector<uint8_t> appendTransportFeedback(const TwccFeedback& fb);

} // namespace crystal
```

创建 `src/media/rtcp/transport_feedback.cpp` 要点：

1. `appendTransportFeedback`：
   - 排序 received/lost 合并出窗口 [baseSeq, baseSeq+count) 的 symbol 序列；count = received+lost 数
   - `refTime 24bit = round(refTimeMs * 64)` 钳 int24
   - delta_i = round((arrival_i - arrival_{i-1}) * 4)（250us 网格），首包 delta = (arrival_0 - refTimeMs)*4；int16 钳制
   - 每 7 个 symbol 一个 StatusVectorChunk：byte0 = 0xC0 | (sym0<<4)|(sym1<<2)|sym2，byte1 = (sym3<<6)|(sym4<<4)|(sym5<<2)|sym6（0xC0 = chunk type=1 + symbol size=2bit）
   - 公共头 length 按 4 字节对齐补零（尾部 padding 计入 length）
2. `rtcp_packet.h`：`RtcpKind` 加 `TransportFeedback`；`RtcpPacket` 加成员 `TwccFeedback twcc;`（include transport_feedback.h）；加 `RTCP_FMT_TWCC` 常量已在 transport_feedback.h。
3. `rtcp_packet.cpp` `parseRtcpCompound` 的 `RTCP_PT_RTPFB` 分支：`fmtOrCount == RTCP_FMT_TWCC` 时解析——读 baseSeq(2B)、statusCount 15 位（byte14 全部 + byte15 高 7 位）、B 位（byte15 最低位，本项目恒 0 不用）、refTime(3B, /64.0 → ms)、fbCount(1B)，然后逐 chunk（首字节高 2 位 0b11 且 symbol size=2bit → 7 symbol/chunk）展开 symbol 流，symbol=01 时随后取 2 字节有符号 delta（/4.0 → ms）累加出到达时刻：`arrival = refTimeMs + Σdelta*0.25`；symbol=00 记 lost。chunk 数不匹配 statusCount 时以先到者为准（防御畸形）。

- [ ] **Step 4: 跑通过 + 提交**

Run: `cmake --build . --target crystal_rtcp_tests -j4 && ./tests/crystal_rtcp_tests`
Expected: 全部 PASSED

```bash
git add src/media/rtcp/transport_feedback.h src/media/rtcp/transport_feedback.cpp src/media/rtcp/rtcp_packet.h src/media/rtcp/rtcp_packet.cpp src/CMakeLists.txt tests/transport_feedback_test.cpp tests/CMakeLists.txt
git commit -m "feat(rtcp): TransportFeedback 编解码——Google TWCC 格式（FMT=15）"
```

---

### Task 3: TrendlineEstimator（延迟梯度线性回归）

**Files:**
- Create: `src/media/gcc/trendline_estimator.h`
- Create: `src/media/gcc/trendline_estimator.cpp`
- Modify: `src/CMakeLists.txt`（新库 crystal_media_gcc）
- Test: `tests/trendline_test.cpp`（新建）

- [ ] **Step 1: 写失败测试**

创建 `tests/trendline_test.cpp`：

```cpp
// ============================================================================
// trendline_test.cpp — Trendline 斜率估计器测试（v2 新增）
// ============================================================================
// 最小二乘线性回归：对 (到达时刻, 累积延迟) 样本求斜率。
// 斜率含义：每毫秒到达时间增加多少毫秒延迟（无量纲，>0 拥塞趋势）。
// ============================================================================
#include <gtest/gtest.h>
#include "media/gcc/trendline_estimator.h"

// 延迟线性增长：每 10ms 到达间隔累积 0.5ms 延迟 → 斜率 ≈ 0.05
TEST(Trendline, IncreasingDelayPositiveSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, i * 0.5);
    EXPECT_GT(est.slope(), 0.03);
    EXPECT_LT(est.slope(), 0.07);
}

// 延迟恒定：斜率 ≈ 0
TEST(Trendline, FlatDelayZeroSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, 5.0);
    EXPECT_NEAR(est.slope(), 0.0, 0.005);
}

// 延迟恢复（下降）：斜率 < 0
TEST(Trendline, DecreasingDelayNegativeSlope) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, 10.0 - i * 0.4);
    EXPECT_LT(est.slope(), -0.02);
}

// 样本不足窗口：不应产出有效斜率（ready() = false）
TEST(Trendline, NotReadyBelowWindow) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 10; ++i)
        est.update(i * 10.0, i * 0.5);
    EXPECT_FALSE(est.ready());
}

// 窗口滑动：老样本滚出后斜率跟上新趋势
TEST(Trendline, SlidingWindow) {
    crystal::TrendlineEstimator est(20);
    for (int i = 0; i < 20; ++i)
        est.update(i * 10.0, i * 0.5);      // 先增长
    for (int i = 20; i < 40; ++i)
        est.update(i * 10.0, 10.0);          // 后平稳
    EXPECT_NEAR(est.slope(), 0.0, 0.02);
}
```

- [ ] **Step 2: 跑失败**

`tests/CMakeLists.txt` 追加目标：

```cmake
add_executable(crystal_gcc_tests
    trendline_test.cpp
)
target_link_libraries(crystal_gcc_tests PRIVATE
    crystal_media_gcc crystal_utils GTest::gtest_main
)
add_test(NAME crystal_gcc_tests COMMAND crystal_gcc_tests)
```

Run: 构建报 `media/gcc/trendline_estimator.h` 不存在
Expected: FAIL

- [ ] **Step 3: 实现**

`src/CMakeLists.txt` 在 monitor 库之后追加：

```cmake
# --- Media: GCC（工程化升级 v2：带宽估计）---
add_library(crystal_media_gcc STATIC
    media/gcc/trendline_estimator.cpp
)
target_include_directories(crystal_media_gcc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(crystal_media_gcc PUBLIC crystal_utils)
```

`src/media/gcc/trendline_estimator.h`：

```cpp
// ============================================================================
// trendline_estimator.h - Trendline 延迟梯度估计器（v2 新增）
// ============================================================================
//【为什么用斜率不用原始 OWD】
// 收发两端时钟不同步，原始单向延迟（OWD）的绝对值无意义（含未知时钟偏移）。
// 但"相邻包 OWD 的差分"消掉了偏移：排队延迟增加 → OWD 在增长 → 拥塞。
// 对 (到达时刻, 累积 OWD 差分) 做最小二乘回归，斜率即延迟增长率。
// libwebrtc 同款思路（modules/remote_bitrate_estimator/trendline_estimator.cc）。
#pragma once

#include <cstdint>
#include <deque>

namespace crystal {

class TrendlineEstimator {
public:
    explicit TrendlineEstimator(size_t window = 20);

    // 喂一个样本：arrivalMs 到达时刻，delayMs 累积延迟（均为差分后数值）
    void update(double arrivalMs, double delayMs);

    // 回归斜率（delay 增量 / 到达时刻增量，>0 延迟在涨）
    double slope() const;

    // 样本数达到窗口要求了吗（不足时 slope() 返回 0）
    bool ready() const;

private:
    size_t window_;                       // 样本窗口
    std::deque<double> t_, y_;            // 样本
    double sumT_ = 0, sumY_ = 0;          // 滚动和（避免每次全量重算）
    double sumTT_ = 0, sumTY_ = 0;
};

} // namespace crystal
```

`src/media/gcc/trendline_estimator.cpp`：`update` 追加样本并维护 4 个滚动和，超窗口从队头扣除；`slope` 按公式 `Σ(t-t̄)(y-ȳ)/Σ(t-t̄)² = (n·Σty - Σt·Σy)/(n·Σt² - (Σt)²)` 计算，分母为 0 返回 0。

- [ ] **Step 4: 跑通过 + 提交**

Run: `cmake --build . --target crystal_gcc_tests -j4 && ./tests/crystal_gcc_tests`
Expected: 5 PASSED

```bash
git add src/media/gcc/trendline_estimator.h src/media/gcc/trendline_estimator.cpp src/CMakeLists.txt tests/trendline_test.cpp tests/CMakeLists.txt
git commit -m "feat(gcc): Trendline 估计器——累积延迟最小二乘斜率"
```

---

### Task 4: GccController（过载检测 + AIMD 速率控制）

**Files:**
- Create: `src/media/gcc/gcc_controller.h`
- Create: `src/media/gcc/gcc_controller.cpp`
- Modify: `src/CMakeLists.txt`（gcc 库加源）
- Test: `tests/gcc_controller_test.cpp`（新建）

- [ ] **Step 1: 写失败测试**

创建 `tests/gcc_controller_test.cpp`：

```cpp
// ============================================================================
// gcc_controller_test.cpp — GCC 控制器测试（v2 新增）
// ============================================================================
// 双通道融合：丢包通道（RR）优先级硬规则，趋势通道（TWCC→trendline）做
// 精细化升降。AIMD：过载 ×0.85，正常递增，钳制 [100,4000] kbps。
// ============================================================================
#include <gtest/gtest.h>
#include "media/gcc/gcc_controller.h"

// 初始码率取构造值
TEST(Gcc, InitialBitrate) {
    crystal::GccController gcc(1000);
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
}

// 丢包通道：>10% 强降（无视趋势通道）
TEST(Gcc, LossAbove10Reduces) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.15);
    EXPECT_LT(gcc.targetBitrateKbps(), 1000u);
    EXPECT_NEAR(gcc.targetBitrateKbps(), 850u, 1);  // ×0.85
}

// 丢包通道：2%~10% 持平
TEST(Gcc, LossMidRangeHold) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.05);
    EXPECT_EQ(gcc.targetBitrateKbps(), 1000u);
}

// 丢包通道：<2% 允许递增
TEST(Gcc, LossBelow2AllowsIncrease) {
    crystal::GccController gcc(1000);
    gcc.onLossUpdate(0.01);
    EXPECT_GT(gcc.targetBitrateKbps(), 1000u);
}

// 趋势通道：构造延迟持续增长（每 feedback 一批样本）→ 过载降码率
TEST(Gcc, OveruseTrendlineReduces) {
    crystal::GccController gcc(1000);
    // 20 个样本：到达间隔 10ms，累积延迟每包 +0.5ms（斜率 0.05 > 阈值 0.01）
    for (int round = 0; round < 3; ++round) {  // 连续 3 轮过载才降（去抖）
        crystal::FeedbackSample s;
        s.baseSeq = 100 + round * 20;
        s.sendMs = round * 200.0;
        s.arrivals.reserve(20);
        for (int i = 0; i < 20; ++i)
            s.arrivals.push_back({static_cast<uint16_t>(100 + round * 20 + i),
                                  s.sendMs + i * 10.0 + 200.0 + i * 0.5});
        gcc.onFeedback(s);
        gcc.tick();  // 每轮结束应用状态机
    }
    EXPECT_LT(gcc.targetBitrateKbps(), 1000u);
}

// 钳制下限 100kbps
TEST(Gcc, ClampsToMinimum) {
    crystal::GccController gcc(150);
    for (int i = 0; i < 10; ++i) gcc.onLossUpdate(0.5);
    EXPECT_EQ(gcc.targetBitrateKbps(), 100u);
}

// 钳制上限 4000kbps
TEST(Gcc, ClampsToMaximum) {
    crystal::GccController gcc(3900);
    for (int i = 0; i < 30; ++i) gcc.onLossUpdate(0.0);
    EXPECT_EQ(gcc.targetBitrateKbps(), 4000u);
}
```

- [ ] **Step 2: 跑失败**

`tests/CMakeLists.txt` 的 `crystal_gcc_tests` 源列表加 `gcc_controller_test.cpp`。
Run: 构建报 `gcc_controller.h` 不存在
Expected: FAIL

- [ ] **Step 3: 实现**

`src/media/gcc/gcc_controller.h`：

```cpp
// ============================================================================
// gcc_controller.h - GCC 带宽估计控制器（v2 新增）
// ============================================================================
// 双通道（libwebrtc 同款思路）：
//   丢包通道：loss > 10% → 强降（×0.85）；2%~10% → 持平；< 2% → 可增长
//   趋势通道：TWCC feedback → trendline 斜率 → 过载检测 → 精细化降
// AIMD 速率控制：
//   过载：bitrate × 0.85
//   正常增长：+max(40, 8%×bitrate) kbps/次 tick
// 生产差异（指南展开）：自适应过载阈值、ProbeController 探测、PacedSender。
#pragma once

#include "media/gcc/trendline_estimator.h"
#include <cstdint>
#include <vector>

namespace crystal {

// 一次 feedback 的还原样本（main_client 从 RTCP 解出后组装）
struct FeedbackSample {
    uint16_t baseSeq = 0;
    double sendMs = 0;                    // 本批首个包的发送时刻（本地时钟）
    std::vector<ArrivalSample> arrivals;  // (seq, arrivalMs)——已还原到发送端时间轴
};

class GccController {
public:
    explicit GccController(uint32_t initKbps);

    // 丢包通道输入（RR 报告块 fraction lost / 255）
    void onLossUpdate(double lossRate);

    // 趋势通道输入：一批 feedback 样本（内部喂 trendline 累积 OWD 差分）
    void onFeedback(const FeedbackSample& s);

    // 周期 tick（如每 100ms）：应用状态机 + AIMD，产出新目标码率
    void tick();

    uint32_t targetBitrateKbps() const { return targetKbps_; }

private:
    TrendlineEstimator trendline_;
    uint32_t targetKbps_;
    int overuseStreak_ = 0;   // 连续过载计数（≥3 才降，去抖）
    double lastLoss_ = 0;
    // 上一批样本的末尾（OWD 差分跨批衔接用）
    double lastArrival_ = 0, lastDelay_ = 0;
    bool hasLast_ = false;
};

} // namespace crystal
```

`src/media/gcc/gcc_controller.cpp` 要点：

1. `onFeedback`：对 arrivals 按 seq 排序，逐包计算 OWD 差分样本：
   - `owd_i = arrival_i - sendMs - (i 对应的发送推进)`——发送端每包发送时刻 = `sendMs + (i × 包间隔)` 未知，**简化：假设本批内发包等间隔（用 arrivals 数量近似），OWD_i = arrival_i - sendMs - i×(批时长/包数)` 太绕。改为直接用一阶差分：`delay_i = (arrival_i - arrival_{i-1})`，与发送端包间隔无关的做法是把发送间隔也差分——**最终简化（正确且简单）：发送端记录每包 sendTime，FeedbackSample 直接带每包 (seq, arrivalMs)，发送时刻由 main_client 在组装时算好 `owdMs = arrivalMs - sendTimeMs`（同机时钟，feedback 到达后还原）。**（对应 main_client Task 5：本地 map<seq, sendMs> + feedback arrival → owd。FeedbackSample 改为携带 `std::vector<std::pair<uint16_t,double>> owdSamples`？——**修正接口见下**）
   - 累积延迟 `accum += owd_i - owd_{i-1}`，喂 `trendline_.update(arrival_i, accum)`
2. **接口修正（实现前定稿）**：`FeedbackSample` 改为：
   ```cpp
   struct FeedbackSample {
       std::vector<std::pair<uint16_t, uint16_t>> seqs;  // (seq, arrivalMs量化)
       std::vector<double> owdMs;                        // 与 seqs 对齐的单向延迟（ms）
   };
   ```
   main_client 组装：arrival 来自 feedback 解码，sendTime 来自本地记录，owd = arrival - sendTime（同机时钟直接可减）。GCC 内部做差分累积。
3. `onLossUpdate`：>0.10 → `targetKbps_ × 0.85`（直接生效，硬通道）；0.02~0.10 → 不变；< 0.02 → 只置标志（增长在 tick 里做）。记录 `lastLoss_`。
4. `tick()`：
   - 趋势通道：`trendline_.ready() && slope() > 0.01` → overuseStreak_++；slope < -0.01 → streak=0（恢复）；否则不变。streak ≥ 3 → `targetKbps_ × 0.85`，streak 归零
   - 增长：非过载且 lastLoss_ < 0.02 → `+max(40, targetKbps_×8/100)` kbps
   - 钳制 [100, 4000]

- [ ] **Step 4: 跑通过 + 提交**

Run: `cmake --build . --target crystal_gcc_tests -j4 && ./tests/crystal_gcc_tests`
Expected: 全部 PASSED

```bash
git add src/media/gcc/gcc_controller.h src/media/gcc/gcc_controller.cpp src/CMakeLists.txt tests/gcc_controller_test.cpp tests/CMakeLists.txt
git commit -m "feat(gcc): GccController——双通道融合过载检测+AIMD 速率控制"
```

---

### Task 5: 全链路接线（packetizer TWCC + setBitrate + main_client + Demo）

**Files:**
- Modify: `src/media/rtp/rtp_packetizer.h/.cpp`（packetize 后自动打 TWCC 序号）
- Modify: `src/media/video/h264_encoder.h/.cpp`（setBitrate）
- Create: `src/media/rtcp/twcc_recorder.h/.cpp`（接收端收集）
- Modify: `main_client.cpp`（收发两端接线）
- Modify: `main_demo.cpp`（Demo 5：虚拟信道验证 GCC）
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: TwccRecorder（接收端）**

`twcc_recorder.h/.cpp`（放 rtcp/ 目录，属于反馈路径）：

```cpp
// 记录到达样本；hasPending() 满 100ms 或窗口满时产出 TwccFeedback
class TwccRecorder {
public:
    explicit TwccRecorder(uint32_t senderSsrc, uint32_t mediaSsrc);
    // 每个 RTP 包到达时调用（onTrack 回调里）
    void onPacket(uint16_t twccSeq, double arrivalMs);
    // 周期调用：距首样本 ≥100ms 或积压 ≥64 包时返回 feedback 并清窗口
    bool buildFeedback(double nowMs, TwccFeedback& out);
private:
    ...
};
```

内部：`std::map<uint16_t,double> pending_`；buildFeedback 取窗口 [minSeq, maxSeq] 补齐中间缺口为 lost，refTime = 首样本 arrival，产出后清空。测试加入 `tests/transport_feedback_test.cpp` 追加（Recorder 100ms 窗口行为 + 缺口补 lost）。

- [ ] **Step 2: RtpPacketizer 打 TWCC**

`packetizeH264/packetizeOpus` 内部：每生成一个包 `setTwccSeq(twccSeq_++)`（构造函数起始值随机）。**只对视频流启用**——加构造参数 `bool enableTwcc = false`（main_client 视频传 true，音频 false——音频包小且 50/s，反馈开销不划算，生产同样只做视频）。

- [ ] **Step 3: H264Encoder::setBitrate**

```cpp
// setBitrate - 运行时调整目标码率（GCC 输出应用点，v2 新增）
// 直接改 codecCtx_->bit_rate；libx264 的 RC（码率控制）会在后续帧
// 渐进收敛到新目标（不重置编码器，无黑帧），质量过渡有短暂波动。
// 钳制 [100, 4000] kbps 与 GccController 的输出范围一致。
bool H264Encoder::setBitrate(uint32_t kbps);
```

- [ ] **Step 4: main_client 接线**

发送端：
- `RtpPacketizer videoPacketizer(96, 90000, videoSsrc, seq, 1200, /*enableTwcc=*/true)`
- `std::map<uint16_t, double> twccSendTimes_;` 发送媒体包时（`sendMedia` 前）记录 `(twccSeq, nowMs())`，容量上限 512 滚动淘汰
- `onRtcp` 加 `TransportFeedback` 分支：解出 `p.twcc` → 组装 `FeedbackSample`（对每个 received 样本查 `twccSendTimes_` 得 owdMs）→ `gcc.onFeedback(s)`
- 16ms 轮询循环里每 ~100ms：`gcc.tick()` → `encoder.setBitrate(gcc.targetBitrateKbps())`；RR 到达时 `gcc.onLossUpdate(videoRecvReport...)`（用对端观测我方视频流的丢包率：`videoSendReport.remoteFractionLost()/255.0`）
- [stats] 行追加 `| GCC {kbps}kbps slope {x.xx}` 输出

接收端：
- onTrack 视频分支：`pkt.getTwccSeq(seq)` 成功 → `twccRecorder.onPacket(seq, nowMs())`
- 16ms 轮询里每 ~100ms：`twccRecorder.buildFeedback(nowMs(), fb)` → `appendTransportFeedback` → `pc->sendRtcp()`

- [ ] **Step 5: Demo 5（无硬件验证 GCC）**

`main_demo.cpp` 新增 `demoGcc()`：
- 造 60 个虚拟包样本（发送间隔 16ms），分两段：前 30 包到达延迟平稳（slope≈0），后 30 包每包多 +0.6ms 排队延迟（模拟带宽劣化）
- 喂 GccController → 打印每 tick 目标码率，观察"前段爬升 → 后段 ×0.85 下降"的完整 AIMD 曲线
- 无需网络/硬件，纯内存验证状态机

- [ ] **Step 6: 全量构建 + 测试 + 提交**

Run: `cd /workspace/build && cmake --build . -j4 && ctest && ./demo | tail -30`
Expected: 5 套测试全 PASSED；demo 输出 GCC 曲线

```bash
git add -A src/ tests/ main_client.cpp main_demo.cpp
git commit -m "feat(gcc): 全链路接线——TWCC 打点/反馈回传/码率应用 + Demo 虚拟信道验证"
```

---

### Task 6: LEARNING_GUIDE.md 新增 Module 11

**Files:**
- Modify: `docs/LEARNING_GUIDE.md`

- [ ] **Step 1: 目录加 Module 11 链接（Module 13 之后、面试专题之前）**

- [ ] **Step 2: 写 Module 11 完整内容**（在 Module 13 章节之后插入），含五节：

1. **概念讲解**：GCC 解决什么（UDP 无拥塞控制，发送方不知道网络满了）；双通道架构图；为什么弃用 REMB（接收端估带宽只看丢包，慢且粗）选 TWCC（发送端拿到每包到达时刻，延迟趋势提前于丢包发生）
2. **逐组件精读**：TWCC 扩展头字节图（对照 rtp_packet.cpp）；TransportFeedback 报文字节图 + 本实现"只 2-bit symbol + 2B delta"简化 vs 生产的 RunLength 压缩；Trendline 公式推导（一阶差分消时钟偏移的数学：OWD_i = 静态偏移 + 排队延迟_i，差分后偏移消掉）+ 最小二乘；过载状态机（为什么 3 轮去抖）+ AIMD 参数（×0.85 / +8% 的量级理由）
3. **动手练习**：改 AIMD 参数观察收敛速度；造 slope 数据触发 OVERUSE；用 netem 实测（附 `tc` 命令）
4. **面试高频题**（≥5）：为什么用斜率不用原始 OWD；REMB vs TWCC；AIMD 为什么乘性降加性增；GCC 两个通道优先级；TWCC 扩展头格式
5. **生产差异表**：自适应阈值 / Probe / Pacer / ASRE（多流同步）

- [ ] **Step 3: 提交**

```bash
git add docs/LEARNING_GUIDE.md
git commit -m "docs: Module 11 GCC 带宽估计（原理→字节图→公式推导→面试题）"
```

---

## Self-Review 记录

1. **Spec 覆盖**：设计文档 6.1 五组件 ✓（Task 1 TWCC 头、Task 2 feedback 包、Task 5 recorder、Task 3 trendline、Task 4 gcc_controller）+ 6.2 setBitrate ✓（Task 5）+ 6.3 验证 ✓（Demo 5 + 测试）+ 7 指南 ✓（Task 6）
2. **顺序调整**：用户确认 GCC 提前（原 Phase D），延迟与线程顺延——计划开头已注明
3. **接口定稿**：Task 4 实现说明里修正了 FeedbackSample 接口（owd 在 main_client 组装，避免 GCC 内部猜发送间隔）——实现时按修正版
4. **测试设计**：每个组件 round-trip / 造数据 / 边界钳制三类，共 3+3+5+7+~14 个新测试
5. **占位符扫描**：无 TBD/TODO；Task 5 Step 4 接线细节以要点列出（接线类任务代码高度依赖现有回调结构，逐行代码在实现时贴齐）
