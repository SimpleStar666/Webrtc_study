// ============================================================================
// jitter_buffer.cpp - 抖动缓冲区实现
// ============================================================================
//
// 本文件实现了 JitterBuffer 类的核心逻辑，包括 RTP 包的插入、丢包检测、
// 重排序和消费输出。
//
// 【核心设计 - 两个序列号指针的分离】
//
// expectedSeq_   - 由 insert() 独占维护，用于丢包检测
// nextOutputSeq_ - 由 consume() 独占维护，用于排序输出
//
// 两者互不干扰，避免了 consume() 回退指针导致 insert() 丢包误判的问题。
//
// 【核心算法 - 序列号差值计算与回绕处理】
//
// RTP 序列号是 16 位无符号整数（0-65535），在比较序列号大小时必须正确
// 处理回绕（wraparound）问题。本实现使用的关键技巧是：
//
//   int16_t diff = static_cast<int16_t>(seq - expectedSeq);
//
// 原理：
//   当 seq 和 expectedSeq 都是 uint16_t 时，seq - expectedSeq 的结果
//   也是 uint16_t（模 65536 运算）。将这个结果强制转换为 int16_t 后，
//   由于二进制补码的表示方式：
//     - 值 0~32767 表示 seq >= expectedSeq（正常递增或相等）
//     - 值 32768~65535（即 int16_t 的 -32768~-1）表示 seq < expectedSeq（乱序/重复）
//
// 示例（expectedSeq_ 代表"下一个期望收到的序列号"）：
//   expectedSeq=101, seq=101 → diff=0   （收到期望的包，正常）
//   expectedSeq=101, seq=102 → diff=1   （跳了1个包，seq 101 丢失）
//   expectedSeq=101, seq=103 → diff=2   （跳了2个包，seq 101,102 丢失）
//   expectedSeq=65535, seq=0 → diff=1   （回绕，跳了1个包）
//   expectedSeq=101, seq=100 → diff=-1  （乱序，迟到的包）
//   expectedSeq=1, seq=65534 → diff=-3  （回绕方向的乱序）
//
// 【核心算法 - 丢包检测（insert 中使用 expectedSeq_）】
//
// 当收到的包的序列号大于期望序列号时，说明中间有包缺失：
//   diff = seq - expectedSeq_
//   丢包数 = diff
//
// 例如：期望 seq=101，收到 seq=103，则 diff=2，丢包数=2（seq 101, 102 丢失）
//
// 【核心算法 - 排序输出（consume 中使用 nextOutputSeq_）】
//
// consume() 从缓冲区中取出可以安全输出的包，策略如下：
//   1. diff <= 0: 包已到或迟到，直接输出
//   2. diff <= 3: 间隙较小（1-3个包），判定为丢包，不再等待，直接输出
//   3. diff > 3:  间隙较大，可能包还在路上，跳过等待
//
// 阈值 3 的含义：如果连续缺失 3 个以上的包，很可能不是乱序而是真的丢包，
// 此时应该输出已有的包，避免延迟过大。
// ============================================================================

#include "media/rtp/jitter_buffer.h"
#include "utils/logger.h"

namespace crystal {

// ============================================================================
// 构造函数
// ============================================================================
JitterBuffer::JitterBuffer(uint32_t targetDelayMs)
    : targetDelayMs_(targetDelayMs) {}

// ============================================================================
// insert - 向缓冲区插入 RTP 包
// ============================================================================
//
// 【算法流程】
// 1. 递增接收计数器
// 2. 如果是第一个包，同时初始化 expectedSeq_ 和 nextOutputSeq_，插入后返回
// 3. 计算序列号差值 diff（有符号，正确处理回绕）
// 4. 根据 diff 的值进行不同处理：
//    - diff == 0: 收到期望的包，正常插入，expectedSeq_ 前进
//    - diff > 0:  收到未来的包，检测间隙并估算丢包数，expectedSeq_ 跳过间隙
//    - diff < 0:  收到迟到/重复的包，仅插入缓冲区，不更新 expectedSeq_
//
// 【expectedSeq_ 的语义】
// expectedSeq_ 代表"下一个期望从网络收到的序列号"。
// 每收到一个非迟到的包，expectedSeq_ 都会更新为 seq + 1。
// 此变量仅由 insert() 修改，consume() 绝不修改。
//
// 【关于重复包】
// 当 diff < 0 时，包可能是迟到的（之前被判定为丢包）或重复的。
// 无论如何，将其插入缓冲区（如果已存在则覆盖），consume 时会处理。
// 注意：之前已经将间隙计为丢包，这里不做修正（简化处理）。
std::vector<uint16_t> JitterBuffer::insert(const RtpPacket& pkt) {
    // 本次新检测到的丢失序列号（供上层发起 NACK 重传请求）
    std::vector<uint16_t> missing;

    receivedCount_++;

    // ========================================================================
    // 第一个包的特殊处理
    // 同时初始化两个序列号指针：
    //   expectedSeq_   = seq + 1（下一个期望从网络收到的序列号）
    //   nextOutputSeq_ = seq    （当前包就是下一个要输出的）
    // ========================================================================
    if (firstPacket_) {
        expectedSeq_ = pkt.sequenceNumber() + 1;
        nextOutputSeq_ = pkt.sequenceNumber();
        firstPacket_ = false;
        buffer_[pkt.sequenceNumber()] = pkt;
        Logger::debug("JitterBuffer: first packet seq={}", pkt.sequenceNumber());
        return missing;
    }

    uint16_t seq = pkt.sequenceNumber();

    // ========================================================================
    // 计算序列号差值（有符号），正确处理回绕
    // 使用 expectedSeq_（丢包检测指针），而非 nextOutputSeq_
    // ========================================================================
    int16_t diff = static_cast<int16_t>(seq - expectedSeq_);

    if (diff == 0) {
        // ====================================================================
        // 收到期望的包（正常情况）
        // seq 正好等于 expectedSeq_，说明没有丢包，按序到达
        // 插入缓冲区，并将 expectedSeq_ 前进到下一个期望的序列号
        // ====================================================================
        buffer_[seq] = pkt;
        expectedSeq_ = seq + 1;
    } else if (diff > 0) {
        // ====================================================================
        // 收到未来的包（seq > expectedSeq_）
        // 缺失的包数 = diff（即 seq - expectedSeq_）
        // 例如: expectedSeq_=101, seq=103 → 缺失 101,102
        // ====================================================================
        lostCount_ += static_cast<uint64_t>(diff);
        if (diff <= kMaxNackGap) {
            // 小间隙：把缺失的 seq 报告给上层做 NACK 重传请求
            for (int i = 0; i < diff; i++) {
                missing.push_back(static_cast<uint16_t>(expectedSeq_ + i));
            }
        } else {
            // 大间隙：疑似码流中断/重连，请求重传只会造成 NACK 风暴，
            // 画面恢复交给 PLI（关键帧请求）
            Logger::warn("JitterBuffer: huge gap ({}), skip NACK report", diff);
        }
        Logger::debug("JitterBuffer: gap detected, expected {} got {}, {} lost",
                      expectedSeq_, seq, diff);
        buffer_[seq] = pkt;
        expectedSeq_ = seq + 1;
    } else {
        // ====================================================================
        // 收到迟到或重复的包（seq < expectedSeq_）
        // diff < 0 意味着这个包的序列号小于期望值
        //
        // 可能的原因：
        //   1. 包乱序到达（之前被误判为丢包，现在迟到了）
        //   2. 包重复（网络重复发送）
        //
        // 仍然插入缓冲区，consume 时会根据 nextOutputSeq_ 决定是否输出
        // 注意: 之前已经将间隙计为丢包，这里不做修正（简化处理）
        // ====================================================================
        buffer_[seq] = pkt;
        Logger::debug("JitterBuffer: late/duplicate packet seq={}", seq);
    }

    return missing;
}

// ============================================================================
// consume - 从缓冲区消费可输出的 RTP 包
// ============================================================================
//
// 【算法流程】
// 遍历缓冲区（std::map 按序列号排序），对每个包计算其与 nextOutputSeq_ 的差值：
//
// 1. diff <= 0: 包的序列号 <= 期望输出序列号
//    - 说明这个包已经"到期"（可能是之前期望的包，或迟到的包）
//    - 直接输出，更新 nextOutputSeq_ = seq + 1
//
// 2. 0 < diff <= 3: 包的序列号略大于期望输出值，间隙在 1-3 个包以内
//    - 判定为丢包，不再等待缺失的包
//    - 更新 nextOutputSeq_ = seq + 1，输出当前包
//
// 3. diff > 3: 间隙较大（缺失超过 3 个包）
//    - 可能这些包还在路上（网络延迟较大但未丢包）
//    - 跳过，等待后续 insert 后再处理
//
// 【关键：使用 nextOutputSeq_ 而非 expectedSeq_】
// nextOutputSeq_ 是 consume() 独占的输出指针，不会被 insert() 修改，
// 也不会因为回退而影响 insert() 的丢包检测。
std::vector<RtpPacket> JitterBuffer::consume() {
    std::vector<RtpPacket> result;

    // 缓冲区为空，直接返回
    if (buffer_.empty()) return result;

    // 遍历缓冲区中的所有包（按序列号排序）
    auto it = buffer_.begin();
    while (it != buffer_.end()) {
        uint16_t seq = it->first;

        // 使用 nextOutputSeq_（排序输出指针），而非 expectedSeq_
        int16_t diff = static_cast<int16_t>(seq - nextOutputSeq_);

        if (diff <= 0) {
            // ==================================================================
            // 包已到期（序列号 <= 期望输出序列号）
            // 输出该包，更新输出指针
            // ==================================================================
            result.push_back(it->second);
            nextOutputSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else if (diff <= 3) {
            // ==================================================================
            // 间隙较小（1-3个包），判定为丢包，不再等待
            // 跳过缺失的包，输出当前包
            // ==================================================================
            nextOutputSeq_ = seq;
            result.push_back(it->second);
            nextOutputSeq_ = seq + 1;
            it = buffer_.erase(it);
        } else {
            // ==================================================================
            // 间隙较大（>3个包），可能包还在路上
            // 跳过，等待后续包到达后再处理
            // ==================================================================
            ++it;
        }
    }

    return result;
}

// ============================================================================
// 统计方法实现
// ============================================================================

// 获取丢包总数
uint64_t JitterBuffer::lostPacketCount() const { return lostCount_; }

// 获取已接收包总数
uint64_t JitterBuffer::receivedPacketCount() const { return receivedCount_; }

// ============================================================================
// 计算丢包率
// ============================================================================
//
// 丢包率 = 丢包数 / (接收数 + 丢包数)
//
// 分母使用 receivedCount_ + lostCount_ 而非仅 receivedCount_ 的原因：
// 丢包率应该反映"应该收到的包中丢失的比例"，应该收到的包 = 实际收到的 + 丢失的
//
// 例如: 接收了 97 个包，检测到 3 个丢包
//   丢包率 = 3 / (97 + 3) = 3% （正确）
//   如果用 3 / 97 = 3.09% （略有偏差）
double JitterBuffer::lossRate() const {
    uint64_t total = receivedCount_ + lostCount_;
    if (total == 0) return 0.0;
    return static_cast<double>(lostCount_) / static_cast<double>(total);
}

} // namespace crystal
