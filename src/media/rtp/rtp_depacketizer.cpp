// ============================================================================
// rtp_depacketizer.cpp - RTP 解包器实现
// ============================================================================
//
// 本文件实现了 RtpDepacketizer 类的核心解包逻辑，包括 H.264 FU-A 分片的
// 重组和 Opus 帧的直接提取。
//
// 【关键算法 - FU-A 重组】
// FU-A 重组是打包器 FU-A 分片的逆过程：
//
// 打包时: 原始 NAL → [FU Indicator + FU Header + 分片数据] × N 个 RTP 包
// 解包时: N 个 RTP 包 → 重组为原始 NAL
//
// 重组的关键步骤：
// 1. 从 FU Indicator 提取 nal_ref_idc（原始 NAL 的重要性等级）
// 2. 从 FU Header 提取原始 NAL 的 type 和 Start/End 标记
// 3. Start 包时：重建 NAL Header = (nal_ref_idc << 5) | original_type
// 4. 中间包时：追加数据到缓冲区
// 5. End 包时：追加数据，输出完整 NAL 单元
//
// 【NAL Header 重建原理】
// 原始 NAL Header 格式: [F(1)] [NRI(2)] [Type(5)]
// FU Indicator 格式:     [F(1)] [NRI(2)] [Type=28(5)]
// FU Header 格式:        [S(1)] [E(1)] [R(1)] [Type(5)]
//
// 重建: NAL Header = (FU Indicator & 0x60) | (FU Header & 0x1F)
//       即从 FU Indicator 取 NRI，从 FU Header 取原始 Type
//       forbidden_zero_bit 始终为 0
// ============================================================================

#include "media/rtp/rtp_depacketizer.h"
#include "utils/logger.h"

namespace crystal {

// 默认构造函数，fuStarted_ 初始化为 false（空闲状态）
RtpDepacketizer::RtpDepacketizer() = default;

// ============================================================================
// depacketizeH264 - H.264 RTP 包解包
// ============================================================================
//
// 【算法流程】
//
// 1. 提取 RTP 负载的第一个字节的低 5 位，判断 NAL 类型
//
// 2. NAL type 1-23（单 NAL 单元）:
//    - 如果正在重组 FU-A，丢弃不完整的旧缓冲区
//    - 直接输出整个负载作为完整的 NAL 单元
//
// 3. NAL type 28（FU-A 分片）:
//    a) 提取 FU Header 中的 Start/End 位和原始 NAL type
//    b) Start 包（S=1）:
//       - 如果正在重组，丢弃旧数据
//       - 重建 NAL Header 并写入缓冲区
//       - 追加 Start 包的分片数据
//    c) 中间包（S=0, E=0）:
//       - 如果在重组中，追加数据
//       - 如果不在重组中，丢弃
//    d) End 包（E=1）:
//       - 追加数据
//       - 输出完整 NAL 单元
//       - 重置状态
//
// 4. NAL type 24（STAP-A）:
//    - 当前仅记录日志，未完全实现
//
// 【关于 NAL type 的判断】
// RTP 负载的第一个字节就是 NAL Header（单 NAL 模式）或 FU Indicator（FU-A 模式），
// 其低 5 位就是 NAL type：
//   1-23: 单个 NAL 单元（如 1=非IDR切片, 5=IDR切片, 6=SEI, 7=SPS, 8=PPS）
//   24:   STAP-A 聚合包
//   25:   STAP-B 聚合包
//   26:   MTAP16 聚合包
//   27:   MTAP24 聚合包
//   28:   FU-A 分片单元
//   29:   FU-B 分片单元
std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeH264(
    const RtpPacket& pkt) {

    std::vector<std::vector<uint8_t>> result;
    const auto& payload = pkt.payload();

    // 空负载直接返回
    if (payload.empty()) return result;

    // 提取 NAL type：负载第一个字节的低 5 位
    // & 0x1F = 0001 1111，提取 bit4-0
    uint8_t nalType = payload[0] & 0x1F;

    // ========================================================================
    // 情况1: 单 NAL 单元（NAL type 1-23）
    // 这些类型表示 RTP 负载直接包含一个完整的 NAL 单元
    // ========================================================================
    if (nalType >= 1 && nalType <= 23) {
        // 如果当前正在重组 FU-A，说明之前的 FU-A 序列不完整（可能丢包），
        // 丢弃不完整的缓冲区，重置状态
        if (fuStarted_) {
            Logger::warn("Dropping incomplete FU-A buffer on single NAL arrival");
            fuStarted_ = false;
            fuBuffer_.clear();
        }
        // 直接输出整个负载作为完整的 NAL 单元
        result.push_back(payload);
    }
    // ========================================================================
    // 情况2: FU-A 分片（NAL type 28）
    // 需要根据 FU Header 中的 Start/End 位进行重组
    // ========================================================================
    else if (nalType == 28) {
        // FU-A 包至少需要 2 字节（FU Indicator + FU Header）
        if (payload.size() < 2) {
            Logger::warn("FU-A packet too short");
            return result;
        }

        // 提取 FU Header（负载第 2 字节）
        uint8_t fuHeader = payload[1];

        // 提取 Start 位：& 0x80 = 1000 0000，检查最高位
        bool startBit = (fuHeader & 0x80) != 0;

        // 提取 End 位：& 0x40 = 0100 0000，检查次高位
        bool endBit = (fuHeader & 0x40) != 0;

        // 提取原始 NAL type：& 0x1F = 0001 1111，取低 5 位
        uint8_t originalNalType = fuHeader & 0x1F;

        // ------------------------------------------------------------------
        // FU-A Start 包处理
        // 开始一个新的 FU-A 重组过程
        // ------------------------------------------------------------------
        if (startBit) {
            // 如果已经在重组中，说明之前的 FU-A 序列不完整，丢弃旧数据
            if (fuStarted_) {
                Logger::warn("New FU-A start while previous incomplete, dropping old");
            }

            // 进入重组状态
            fuStarted_ = true;
            fuBuffer_.clear();

            // 重建 NAL Header
            // payload[0] 是 FU Indicator，& 0x60 = 0110 0000 提取 nal_ref_idc
            // originalNalType 是从 FU Header 中提取的原始 NAL 类型
            // 重组: NAL Header = nal_ref_idc | original_type
            // 例如: FU Indicator=0x7C (NRI=3, type=28), FU Header=0x85 (S=1, type=5)
            //       → reconstructedNal = 0x60 | 0x05 = 0x65 (NRI=3, type=5, IDR切片)
            uint8_t nalRefIdc = (payload[0] & 0x60);
            uint8_t reconstructedNal = nalRefIdc | originalNalType;
            fuBuffer_.push_back(reconstructedNal);

            // 追加 Start 包的分片数据（跳过 FU Indicator 和 FU Header，共 2 字节）
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }
        }
        // ------------------------------------------------------------------
        // FU-A 中间包或 End 包处理
        // 只有在重组状态中才处理，否则丢弃
        // ------------------------------------------------------------------
        else if (fuStarted_) {
            // 追加分片数据（跳过 FU Indicator 和 FU Header，共 2 字节）
            if (payload.size() > 2) {
                fuBuffer_.insert(fuBuffer_.end(), payload.begin() + 2,
                                 payload.end());
            }

            // 如果是 End 包，完成重组
            if (endBit) {
                fuStarted_ = false;  // 回到空闲状态
                result.push_back(std::move(fuBuffer_));  // 输出完整的 NAL 单元
                fuBuffer_.clear();
            }
        }
        // ------------------------------------------------------------------
        // 异常情况：收到中间/End 包但没有先收到 Start 包
        // 可能是 Start 包丢失，丢弃这些无法重组的包
        // ------------------------------------------------------------------
        else {
            Logger::warn("FU-A middle/end without start, dropping");
        }
    }
    // ========================================================================
    // 情况3: STAP-A 聚合包（NAL type 24）
    // STAP-A 将多个小 NAL 单元聚合在一个 RTP 包中
    // 当前仅记录日志，未完全实现解包逻辑
    // ========================================================================
    else if (nalType == 24) {
        Logger::debug("STAP-A packet received (not yet fully supported, extracting single NAL)");
    }
    // ========================================================================
    // 其他不支持的 NAL 类型
    // ========================================================================
    else {
        Logger::warn("Unsupported NAL type in RTP: {}", static_cast<int>(nalType));
    }

    return result;
}

// ============================================================================
// depacketizeOpus - Opus RTP 包解包
// ============================================================================
//
// 【算法说明】
// Opus 音频帧不需要分片和重组，每个 RTP 包的负载就是一个完整的 Opus 帧。
// 直接提取负载即可。
//
// 【与 H.264 解包的区别】
// Opus 解包是无状态的，不需要维护重组缓冲区，因为：
//   1. Opus 帧很小，不需要 FU-A 分片
//   2. 每个 RTP 包恰好包含一个完整的 Opus 帧
//   3. 不存在跨包重组的需求
std::vector<std::vector<uint8_t>> RtpDepacketizer::depacketizeOpus(
    const RtpPacket& pkt) {
    std::vector<std::vector<uint8_t>> result;
    // 直接将 RTP 负载作为 Opus 帧输出
    result.push_back(pkt.payload());
    return result;
}

} // namespace crystal
