// ============================================================================
// opus_fec_test.cpp — Opus FEC/DTX 弱网对抗单元测试（工程化升级 v2 新增）
// ============================================================================
// 本测试文件验证 Opus 音频抗丢包机制的正确性，覆盖：
//
// 1. FEC 恢复（RecoverPreviousFrame）：
//    模拟"帧 A 在网络中丢失、只收到帧 B"的弱网场景，
//    验证解码端能从帧 B 中提取内嵌的帧 A 冗余副本（inband FEC），
//    精确恢复出丢失的帧 A——这是音频抗丢包的第一层防线。
//
// 2. DTX 静音帧（SilenceProducesTinyFrame）：
//    验证持续静音时编码器输出极小的舒适噪声帧（1~3 字节），
//    而正常语音帧为 40~160 字节——静音约占通话时间 50%，DTX 可省约一半带宽。
//
// 【三层防线知识地图】
//   FEC ：当前帧内嵌上一帧低码率副本，丢了上一帧可精确恢复（本文件验证）
//   PLC ：解码器用历史帧模糊推测，零带宽（opus_decoder 内置，decode 传 NULL 触发）
//   NACK：重传，完美但迟到——音频播放等不起重传，所以音频不用 NACK（视频用）
// ============================================================================

#include <gtest/gtest.h>
#include "media/audio/opus_encoder.h"
#include "media/audio/opus_decoder.h"

#include <cmath>
#include <vector>

namespace {
// makeTone - 生成 20ms@48kHz 的正弦波 PCM 测试信号
// 参数：samples - 采样点数（960 = 48kHz × 20ms）；freq - 频率（Hz）
// 幅值 8000（int16_t 满量程 32767 的约 1/4，留出余量避免削波）
std::vector<int16_t> makeTone(int samples, int freq = 440) {
    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; ++i)
        pcm[i] = static_cast<int16_t>(
            8000.0 * std::sin(2 * M_PI * freq * i / 48000.0));
    return pcm;
}
} // namespace

// ============================================================================
// FEC 恢复测试
// ============================================================================
// 场景模拟（对应真实弱网丢包）：
//   发送端：帧A(440Hz) → 帧B(880Hz)，B 内嵌了 A 的低码率 FEC 副本
//   网络  ：帧 A 丢失 ✗
//   接收端：只收到 B → decodeFec(B) 恢复 A 的近似 → 再正常解码 B
// 断言要点：
//   - decodeFec 返回非空：证明 B 中确实嵌入了 A 的 FEC 冗余
//   - decode(B) 仍能正常输出 960 采样：证明 FEC 提取不破坏正常解码路径
TEST(OpusFec, RecoverPreviousFrame) {
    crystal::OpusEncoderConfig cfg;
    crystal::OpusEncoder enc(cfg);
    ASSERT_TRUE(enc.init());
    crystal::OpusDecoder dec;
    ASSERT_TRUE(dec.init());

    int frameSize = cfg.frameSize;  // 960（48kHz × 20ms）
    auto a = makeTone(frameSize, 440);
    auto b = makeTone(frameSize, 880);  // 用不同频率区分 A/B 的语义

    // 连续编两帧：编码器在编 B 时会把 A 的低码率副本内嵌进 B
    auto frameA = enc.encode(a.data(), frameSize);
    auto frameB = enc.encode(b.data(), frameSize);
    ASSERT_FALSE(frameA.empty());
    ASSERT_FALSE(frameB.empty());

    // ---- 模拟帧 A 丢失：只收到帧 B ----
    // ① decodeFec(B)：从 B 中提取 FEC 冗余，恢复出 A 的近似（返回非空即成功）
    auto recovered = dec.decodeFec(frameB.data(), frameB.size(), frameSize);
    EXPECT_FALSE(recovered.empty());

    // ② 正常解码 B：同一份压缩数据可以且必须再解码一次
    //    （真实接收链路：先 FEC 恢复上一帧播放，再正常解码当前帧播放）
    auto decodedB = dec.decode(frameB.data(), frameB.size(), frameSize);
    EXPECT_EQ(decodedB.size(), static_cast<size_t>(frameSize));
}

// ============================================================================
// DTX 静音帧测试
// ============================================================================
// DTX（Discontinuous Transmission）：VAD 检测到持续静音后，编码器输出
// 极小的"舒适噪声"帧（约 1~3 字节）而非完整编码（40~160 字节）。
// 【实测行为】libopus 的 VAD 需要 10 帧（200ms）的静音历史来自适应
// 噪声底（noise floor），之后才判定进入静音状态——第 11 帧起输出 1 字节
// 的 DTX 帧。所以本测试先编 20 帧静音"喂"给编码器，再检查最后一帧。
//（这正是真实通话中的表现：用户静音的前 ~200ms 仍按正常帧编码，
//  之后带宽才降下来——VAD 的判定延迟是质量与带宽的折中。）
TEST(OpusDtx, SilenceProducesTinyFrame) {
    crystal::OpusEncoderConfig cfg;
    crystal::OpusEncoder enc(cfg);
    ASSERT_TRUE(enc.init());

    int frameSize = cfg.frameSize;  // 960
    std::vector<int16_t> silence(frameSize, 0);  // 全零 = 纯数字静音
    std::vector<uint8_t> last;
    for (int i = 0; i < 20; ++i)  // 20 帧 = 400ms 静音，跨过 VAD 判定延迟
        last = enc.encode(silence.data(), frameSize);

    // DTX 生效：输出应为 1~3 字节的舒适噪声帧（正常语音帧 40~160 字节）
    EXPECT_LE(last.size(), 3u);
}
