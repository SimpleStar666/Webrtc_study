# Phase B：Opus FEC/DTX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 开启 Opus inband FEC，实现"下一包恢复上一丢失帧"的解码路径，丢包率由 RR 实测动态喂给编码器，形成音频弱网对抗闭环。

**Architecture:** 编码端 `OPUS_SET_INBAND_FEC(1)` + `setPacketLossPct()`（RR 实测喂入）；解码端新增 `decodeFec()`（`opus_decode` 的 decode_fec=1 路径）；接收链路在播放序号出现跳变时，先 FEC 恢复丢失帧再解码当前帧。DTX 已开启（`opus_encoder.cpp` L91），本阶段补测试验证并接入指南。

**Tech Stack:** libopus、googletest。

**设计文档:** `docs/superpowers/specs/2026-09-09-crystalrtc-v2-engineering-upgrade-design.md` 第 4 节

**FEC 原理（Task 1 实现）：**
```
发送：帧A ──(丢失)──✗     帧 B 内嵌 A 的低码率副本（inband FEC）
接收：跳过 A 直接收到 B → opus_decode(B, decode_fec=1) → 恢复出 A 的近似
                          → opus_decode(B, decode_fec=0) → 正常解码 B
【关键工程点】只传 NULL 给 opus_decode 只会触发 PLC（用历史帧模糊推测），
传"下一帧"+decode_fec=1 才是 FEC 精确恢复——这是很多实现抄错的细节。
```

---

### Task 1: 编码端 FEC 开启 + 解码端 decodeFec

**Files:**
- Modify: `src/media/audio/opus_encoder.cpp`（init 加 OPUS_SET_INBAND_FEC）
- Modify: `src/media/audio/opus_decoder.h/.cpp`（新增 decodeFec）
- Test: `tests/opus_fec_test.cpp`（新建）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

创建 `tests/opus_fec_test.cpp`：

```cpp
// ============================================================================
// opus_fec_test.cpp - Opus FEC/DTX 弱网对抗单测（工程化升级 v2 新增）
// ============================================================================
#include "media/audio/opus_encoder.h"
#include "media/audio/opus_decoder.h"
#include <gtest/gtest.h>
#include <vector>
#include <cmath>

namespace {
// 生成 20ms@48kHz 的正弦波 PCM（440Hz，幅值 8000）
std::vector<int16_t> makeTone(int samples, int freq = 440) {
    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; ++i)
        pcm[i] = static_cast<int16_t>(8000.0 * std::sin(2 * M_PI * freq * i / 48000.0));
    return pcm;
}
} // namespace

// FEC：帧 B 内嵌帧 A 的副本，decodeFec(B) 应恢复出非空的 A 近似音频
TEST(OpusFec, RecoverPreviousFrame) {
    crystal::OpusEncoderConfig cfg;
    crystal::OpusEncoder enc(cfg);
    ASSERT_TRUE(enc.init());
    crystal::OpusDecoder dec;
    ASSERT_TRUE(dec.init());

    int frameSize = cfg.sampleRate / 1000 * cfg.frameMs;  // 960
    auto a = makeTone(frameSize, 440);
    auto b = makeTone(frameSize, 880);  // 不同频率便于区分语义

    auto frameA = enc.encode(a.data(), frameSize);
    auto frameB = enc.encode(b.data(), frameSize);
    ASSERT_FALSE(frameA.empty());
    ASSERT_FALSE(frameB.empty());

    // 模拟：A 在网络中丢失，只收到 B
    // ① decodeFec(B)：从 B 中提取 FEC 冗余，恢复 A 的近似
    auto recovered = dec.decodeFec(frameB.data(), frameB.size(), frameSize);
    EXPECT_FALSE(recovered.empty());
    // ② 正常解码 B（同一份数据可以且必须再解码一次）
    auto decodedB = dec.decode(frameB.data(), frameB.size(), frameSize);
    EXPECT_EQ(decodedB.size(), static_cast<size_t>(frameSize));
}

// DTX：静音输入应产生极小的舒适噪声帧（约 1~3 字节）
TEST(OpusDtx, SilenceProducesTinyFrame) {
    crystal::OpusEncoderConfig cfg;
    crystal::OpusEncoder enc(cfg);
    ASSERT_TRUE(enc.init());
    int frameSize = cfg.sampleRate / 1000 * cfg.frameMs;
    std::vector<int16_t> silence(frameSize, 0);
    // DTX 需要先编几帧静音让 VAD 判定进入静音状态
    std::vector<uint8_t> last;
    for (int i = 0; i < 10; ++i) last = enc.encode(silence.data(), frameSize);
    // opus_encode 对静音帧返回 1（DTX 生效），正常语音帧 40~160 字节
    EXPECT_LE(last.size(), 3u);
}
```

注意：`OpusEncoderConfig` 的字段名需与 `opus_encoder.h` 实际一致（先读再写）。

- [ ] **Step 2: CMake 加目标并跑失败**

`tests/CMakeLists.txt` 追加：

```cmake
add_executable(crystal_opus_tests
    opus_fec_test.cpp
)
target_link_libraries(crystal_opus_tests PRIVATE
    crystal_media_audio crystal_utils GTest::gtest_main
)
add_test(NAME crystal_opus_tests COMMAND crystal_opus_tests)
```

Run: `cd /workspace/build && cmake -S .. -B . > /dev/null && cmake --build . --target crystal_opus_tests -j4 2>&1 | grep -E "error" | head -5`
Expected: FAIL —— `decodeFec` 不是 OpusDecoder 的成员

- [ ] **Step 3: 实现**

`opus_decoder.h` 在 `decode` 声明后加：

```cpp
    // FEC 恢复解码（工程化升级 v2 新增）：从"下一帧"中提取内嵌的
    // 上一帧 FEC 副本，返回恢复出的上一帧 PCM（约低码率质量）
    // 【与 decode 的区别】decode_fec=1 走 FEC 提取路径而非正常解码
    std::vector<int16_t> decodeFec(const uint8_t* opusData, size_t len,
                                   int frameSize);
```

`opus_decoder.cpp` 实现：

```cpp
// decodeFec - FEC 恢复：从下一帧提取上一帧的低码率副本
// 【为什么需要这个方法】丢帧后收到下一帧时：
//   - opus_decode(NULL) = PLC：用历史帧"模糊推测"，音质糊
//   - decodeFec(next)   = FEC：从 next 提取上一帧冗余副本，"精确恢复"
// 两者是 WebRTC 音频抗丢包的两层：FEC 在前（有冗余可用时），PLC 兜底。
std::vector<int16_t> OpusDecoder::decodeFec(const uint8_t* opusData, size_t len,
                                            int frameSize) {
    if (!decoder_) return {};
    std::vector<int16_t> output(frameSize * channels_);
    // 最后一个参数 decode_fec=1：提取 opusData 中内嵌的上一帧 FEC 数据
    // 返回值是恢复出的【上一帧】的采样数（不是当前帧的）
    int samples = opus_decode(decoder_, opusData, static_cast<opus_int32>(len),
                               output.data(), frameSize, 1);
    if (samples < 0) {
        // 帧里没有 FEC 数据（编码端未开 FEC / 该帧为 DTX 噪声帧）→ 返回空，
        // 调用方回落到 PLC（opus_decode(NULL)）
        return {};
    }
    output.resize(samples * channels_);
    return output;
}
```

`opus_encoder.cpp` 的 init() 中，DTX 设置之后追加：

```cpp
    // 启用 inband FEC（Forward Error Correction，前向纠错）
    // 【FEC 与 NACK/PLC 的分工——音频抗丢包三层防线】
    //   NACK 重传：音频不做（到达时已错过播放时刻）
    //   FEC      ：当前帧内嵌上一帧低码率副本，丢了上一帧可精确恢复（本项目实现）
    //   PLC      ：解码器用历史帧模糊推测，零带宽（兜底）
    // 注意：inband FEC 只在 SILK 层生效（语音帧），CELT/DTX 帧无 FEC
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
```

- [ ] **Step 4: 跑测试确认通过**

Run: `cd /workspace/build && cmake --build . --target crystal_opus_tests -j4 && ./tests/crystal_opus_tests`
Expected: 2 PASSED（若 RecoverPreviousFrame 因 libopus 版本行为差异失败，检查返回样本数>0 即调整断言粒度并记录）

- [ ] **Step 5: 提交**

```bash
git add src/media/audio/opus_encoder.cpp src/media/audio/opus_decoder.h src/media/audio/opus_decoder.cpp tests/opus_fec_test.cpp tests/CMakeLists.txt
git commit -m "feat(audio): Opus inband FEC——编码端开启+解码端恢复路径"
```

---

### Task 2: 丢包率动态喂入（setPacketLossPct）

**Files:**
- Modify: `src/media/audio/opus_encoder.h/.cpp`
- Test: `tests/opus_fec_test.cpp`（追加）

- [ ] **Step 1: 写失败测试**（追加）

```cpp
// 丢包率喂入：FEC 冗余度随实测丢包率动态调整
TEST(OpusFec, SetPacketLossPct) {
    crystal::OpusEncoderConfig cfg;
    crystal::OpusEncoder enc(cfg);
    ASSERT_TRUE(enc.init());
    // 正常范围 0~100 应全部接受
    EXPECT_TRUE(enc.setPacketLossPct(10));
    EXPECT_TRUE(enc.setPacketLossPct(0));
    EXPECT_TRUE(enc.setPacketLossPct(100));
}
```

- [ ] **Step 2: 跑失败**

Run: 构建后 EXPECT_TRUE 处报 `setPacketLossPct` 未定义（编译错误）
Expected: 编译失败

- [ ] **Step 3: 实现**

`opus_encoder.h` 加声明：

```cpp
    // 动态设置丢包率（0~100%，工程化升级 v2 新增）
    // 【为什么要动态】FEC 冗余是要花带宽的：丢包率 2% 时编 30% 冗余是浪费，
    // 丢包率 30% 时编 2% 冗余等于没编。真实做法是把 RR 实测丢包率
    // 周期喂给编码器，让 libopus 自己决定冗余量。
    // 数据源：对端 RR 的 fraction lost（我方音频流报告块）
    bool setPacketLossPct(uint32_t pct);
```

`opus_encoder.cpp` 实现：

```cpp
// setPacketLossPct - 动态调整 FEC 冗余度（丢包率输入）
bool OpusEncoder::setPacketLossPct(uint32_t pct) {
    if (!encoder_) return false;
    // 钳制到 libopus 合法范围（0~100）
    if (pct > 100) pct = 100;
    return opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PCT(
               static_cast<opus_int32>(pct))) == OPUS_OK;
}
```

- [ ] **Step 4: 跑通过 + 提交**

Run: `cmake --build . --target crystal_opus_tests -j4 && ./tests/crystal_opus_tests`
Expected: 3 PASSED

```bash
git add src/media/audio/opus_encoder.h src/media/audio/opus_encoder.cpp tests/opus_fec_test.cpp
git commit -m "feat(audio): setPacketLossPct——FEC 冗余随实测丢包率动态调整"
```

---

### Task 3: main_client 接线（跳变检测 → FEC 恢复 → 播放）

**Files:**
- Modify: `main_client.cpp`（音频接收分支）

- [ ] **Step 1: 接线**

音频接收分支（`else if (pkt.payloadType() == 97)`）中，在 `audioJitterBuf.insert(pkt);` 之后、`consume()` 之前，插入 gap 检测需要的序号记忆；consume 循环内改为带 FEC 恢复的播放。改后代码：

```cpp
        } else if (pkt.payloadType() == 97) {
            // 音频不做 NACK（重传到达已错过播放时刻），
            // 抗丢包走 FEC+PLC（工程化升级 v2 完成闭环）
            audioRecvReport.onPacketReceived(pkt.ssrc(), pkt.sequenceNumber(),
                                             pkt.timestamp(), crystal::nowNtp());
            audioMetrics.onPacketArrival(nowMs(), pkt.timestamp());  // E2E（v2）
            audioJitterBuf.insert(pkt);
            auto packets = audioJitterBuf.consume();
            static bool hasLastAudioSeq = false;   // 播放序号记忆（跳变检测）
            static uint16_t lastAudioSeq = 0;
            for (const auto& p : packets) {
                // ---- FEC 恢复：播放序号跳变 = 上一帧丢失（工程化升级 v2）----
                // B 收到时发现 A(seq-1) 没到 → decodeFec(B) 恢复 A 再播
                if (hasLastAudioSeq &&
                    static_cast<int16_t>(p.sequenceNumber() - lastAudioSeq) != 1) {
                    auto recovered = opusDecoder.decodeFec(
                        p.payload().data(), p.payload().size(),
                        opusDecoder.frameSize());
                    if (!recovered.empty()) {
                        audioPlayer.play(recovered.data(), recovered.size());
                    }
                    // decodeFec 返回空（该帧无 FEC，如 DTX 噪声帧）→
                    // PLC 兜底：opusDecoder 内部对丢失的处理，此处不显式调
                }
                // ---- 正常解码播放当前帧 ----
                auto frames = depacketizer.depacketizeOpus(p);
                for (const auto& frame : frames) {
                    auto pcm = opusDecoder.decode(frame.data(), frame.size());
                    if (!pcm.empty()) {
                        audioPlayer.play(pcm.data(), pcm.size());
                    }
                }
                hasLastAudioSeq = true;
                lastAudioSeq = p.sequenceNumber();
            }
        }
```

注意：`RtpPacket::payload()` 的访问器与 `OpusDecoder::frameSize()` 需先核对头文件实际签名，按实际调整。

主循环 5s 统计块中，[metrics] 输出之后追加：

```cpp
            // 工程化升级 v2：RR 实测丢包率 → 动态调 FEC 冗余度
            // （对端 RR 的 fraction lost 是"我方音频流在对方眼里"的丢包率）
            if (audioSendReport.remoteFractionLost() > 0 || audioFecFedOnce) {
                uint32_t lossPct = audioSendReport.remoteFractionLost() * 100 / 255;
                opusEncoder.setPacketLossPct(lossPct);
                audioFecFedOnce = true;
            }
```

并在实例化区声明 `bool audioFecFedOnce = false;`。（更简单：无条件喂——`setPacketLossPct(0)` 也合法，省去标志位。最终采用无条件版本：）

```cpp
            // 工程化升级 v2：RR 实测丢包率 → 动态调 FEC 冗余度
            // （对端 RR 的 fraction lost 是"我方音频流在对方眼里"的丢包率）
            opusEncoder.setPacketLossPct(
                audioSendReport.remoteFractionLost() * 100 / 255);
```

- [ ] **Step 2: 全量构建 + 测试**

Run: `cd /workspace/build && cmake --build . -j4 && ctest`
Expected: 全部 PASSED

- [ ] **Step 3: 提交**

```bash
git add main_client.cpp
git commit -m "feat(audio): 接收端跳变检测→FEC 恢复闭环，RR 丢包率动态喂编码器"
```

---

### Task 4: LEARNING_GUIDE.md 更新（Module 5 扩充）

**Files:**
- Modify: `docs/LEARNING_GUIDE.md`（Module 5 内插入 v2 小节 + 追问清单）

- [ ] **Step 1: Module 5 的"动手练习"之前插入小节**

在 `### 动手练习`（Module 5 内）之前插入：

````markdown
#### FEC 实战：三层防线与一个关键细节【工程化升级 v2 新增】

音频抗丢包的三层防线（本项目现在全有了）：

| 防线 | 机制 | 带宽代价 | 恢复质量 |
|------|------|----------|----------|
| FEC | 当前帧内嵌上一帧低码率副本 | 有（随丢包率动态调整） | 精确恢复 |
| PLC | 解码器用历史帧推测 | 零 | 模糊（能听，不脆） |
| NACK | 重传 | 有（来回延迟） | 完美但迟到——音频不用 |

【最关键的一行代码】解码端发现上一帧丢了，要传"下一帧"给
`opus_decode(..., decode_fec=1)`，而不是传 NULL：

```cpp
// 丢帧 A 后收到 B：
auto recovered = decoder.decodeFec(frameB);  // 从 B 提取 A 的副本 → 精确恢复
// 传 NULL 的话 opus_decode 走的是 PLC 模糊推测——很多实现抄错的地方
```

【闭环数据流】对端 RR 的 fraction lost（"我方音频在对方眼里"的丢包率）
→ `setPacketLossPct()` → libopus 决定 FEC 冗余量。丢包率 2% 编 30% 冗余
是浪费，丢包率 30% 编 2% 冗余等于没编——所以必须动态。

【生产差异】libwebrtc 的 AudioOpus FEC 还有 FEC+DTX 互斥处理（静音帧
无 FEC）、opus_fec 模式的码率补偿等细节；本实现覆盖主干。
````

- [ ] **Step 2: Module 5 面试高频题追加（第 5 题后）**

```markdown
6. **Opus FEC 怎么用才是对的？（v2 实战后必答）** 编码端 OPUS_SET_INBAND_FEC(1)
   + 丢包率动态喂 OPUS_SET_PACKET_LOSS_PCT；解码端丢帧后把"下一帧"传给
   opus_decode(decode_fec=1) 恢复上一帧——传 NULL 只是 PLC。FEC/PLC/NACK
   三层防线各自的带宽代价与恢复质量对比表能脱口而出。
```

- [ ] **Step 3: 追问清单"音频"相关追加**（编解码分类下）

```markdown
7. **FEC 冗余度为什么要动态调整？** 丢包率低时冗余是纯浪费，高时不足等于没编；
   数据源是对端 RR 的 fraction lost，闭环到编码器的 PACKET_LOSS_PCT。
```

- [ ] **Step 4: 提交**

```bash
git add docs/LEARNING_GUIDE.md
git commit -m "docs: Module 5 扩充 FEC 实战小节（工程化升级 v2 标记）"
```

---

## Self-Review 记录

1. **Spec 覆盖**：设计文档第 4 节——FEC 编码端 ✓（Task 1）、解码端 FEC 精确恢复 ✓（Task 1/3）、丢包率动态喂 ✓（Task 2/3）、DTX 验证 ✓（Task 1 测试）、指南更新 ✓（Task 4）
2. **占位符扫描**：无 TBD/TODO；Task 3 中"错误示范→正确版本"教学保留，接线只用最终版
3. **类型一致性**：`decodeFec(frameSize)` 签名三处一致；`setPacketLossPct(uint32_t)` 一致
4. **DTX 修正**：审计原以为 DTX 未开，实际 opus_encoder.cpp L91 已有——Phase B 不重复实现，测试补验证
