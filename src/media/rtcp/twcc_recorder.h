// ============================================================================
// twcc_recorder.h - TWCC 到达时刻记录器（工程化升级 v2 新增）
// ============================================================================
//【在 GCC 中的角色】
// 接收端组件：每个携带 TWCC 扩展头的 RTP 包到达时记一笔 (序号, 到达时刻)，
// 攒够一个窗口（≥100ms 或 ≥64 包）就产出一份 TwccFeedback 发回发送端。
// 它是 GCC 趋势通道的数据源头——没有到达时刻，就没有延迟梯度。
//
//【时间基准——本实现的关键设计】
// 到达时刻用的是接收端本地时钟（steady 毫秒，数值巨大）。而 feedback 报文
// 的 refTime 字段只有 24 位（1/64ms 网格，上限约 262 秒），直接编码绝对
// 时刻必然饱和。因此 recorder 内部维护"会话时钟"：以首包到达时刻为原点，
// 编码时全部换算成相对时刻。跨窗口单调（回归的 x 轴不断裂），只在逼近
// 24 位上限时重置原点（约 4 分钟一次，3 轮过载去抖可吸收单次跳变）。
// 发送端还原的 arrival 与本地发送时刻做差得到 OWD——两端时钟偏移是常量，
// 在 GccController 的差分里被消掉。
//
//【为什么 100ms 窗口】
// libwebrtc 同量级：太短则反馈包自身成为带宽开销，太长则拥塞响应迟钝。
// 100ms 足以覆盖几十个视频包（30fps × FU-A 分片），又把控制回路延迟
// 压在一两个帧间隔内。
#pragma once

#include "media/rtcp/transport_feedback.h"
#include <cstdint>
#include <map>

namespace crystal {

class TwccRecorder {
public:
    // senderSsrc：本端（feedback 发起方）SSRC
    // mediaSsrc：被观测的远端媒体流 SSRC（运行时发现后可更新）
    explicit TwccRecorder(uint32_t senderSsrc, uint32_t mediaSsrc);

    // 远端视频流 SSRC 确认后更新（收到对端首包/SR 时才知道）
    void setMediaSsrc(uint32_t ssrc);

    // 每个 RTP 包到达时调用（onTrack 回调里，解析出 TWCC 序号后）
    // arrivalMs 为接收端本地时钟（steady 毫秒）
    void onPacket(uint16_t twccSeq, double arrivalMs);

    // 周期调用：距窗口首样本 ≥100ms 或积压 ≥64 包时产出 feedback 并清窗口
    // 返回 false 表示窗口未满（或无样本），out 不被写入
    bool buildFeedback(double nowMs, TwccFeedback& out);

private:
    uint32_t senderSsrc_;
    uint32_t mediaSsrc_;

    // 窗口内样本：offset = uint16(seq - 窗口首包 seq)（回绕安全）
    // value = 原始到达时刻（未相对化）
    std::map<size_t, double> pending_;
    uint16_t windowBaseSeq_ = 0;   // 本窗口首包的 TWCC 序号
    double windowStartMs_ = 0;     // 本窗口首包到达时刻（原始时钟）
    double epochBaseMs_ = 0;       // 会话时钟原点（首个历史包到达时刻）
    bool hasEpoch_ = false;
};

} // namespace crystal
