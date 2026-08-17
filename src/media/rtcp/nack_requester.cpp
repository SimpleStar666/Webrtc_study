// ============================================================================
// nack_requester.cpp - NACK 请求状态机实现
// ============================================================================

#include "media/rtcp/nack_requester.h"

namespace crystal {

std::vector<uint16_t> NackRequester::onMissing(const std::vector<uint16_t>& gaps,
                                               uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (uint16_t seq : gaps) {
        auto it = pending_.find(seq);
        if (it != pending_.end()) continue;  // 已在表中，交给 tick 重试
        pending_[seq] = Item{1, nowMs};      // 登记即第 1 次请求
        requested_++;
        toRequest.push_back(seq);
    }
    return toRequest;
}

std::vector<uint16_t> NackRequester::tick(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint16_t> toRequest;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowMs - it->second.lastReqMs < kRetryIntervalMs) {
            ++it;  // 未到重试时间
            continue;
        }
        if (it->second.tries >= kMaxRetries) {
            // 重试耗尽仍未恢复 → 放弃（PLI 兜底恢复画面）
            givenUp_++;
            it = pending_.erase(it);
            continue;
        }
        it->second.tries++;
        it->second.lastReqMs = nowMs;
        requested_++;
        toRequest.push_back(it->first);
        ++it;
    }
    return toRequest;
}

void NackRequester::onReceived(uint16_t seq) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(seq);
}

uint64_t NackRequester::requestedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_;
}

uint64_t NackRequester::givenUpCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return givenUp_;
}

} // namespace crystal
