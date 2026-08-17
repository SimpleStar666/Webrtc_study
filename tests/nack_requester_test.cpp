// ============================================================================
// nack_requester_test.cpp — NACK 请求状态机单元测试
// ============================================================================
// 覆盖：新间隙立即请求、重试节奏与上限、恢复后不再请求、放弃计数。
// ============================================================================
#include <gtest/gtest.h>
#include "media/rtcp/nack_requester.h"

using namespace crystal;

TEST(NackRequesterTest, NewGapRequestsImmediately) {
    NackRequester nack;
    auto req = nack.onMissing({101, 102}, 0);
    EXPECT_EQ(req, (std::vector<uint16_t>{101, 102}));  // 首次请求立即返回
    EXPECT_EQ(nack.requestedCount(), 2u);
}

TEST(NackRequesterTest, RetryTooEarlyReturnsNothing) {
    NackRequester nack;
    nack.onMissing({101}, 0);
    EXPECT_TRUE(nack.tick(10).empty());   // 10ms < 33ms，未到重试时间
    EXPECT_TRUE(nack.tick(32).empty());   // 32ms 仍未到
}

TEST(NackRequesterTest, RetriesUpToLimitThenGivesUp) {
    NackRequester nack;
    nack.onMissing({101}, 0);                                 // 第 1 次请求
    EXPECT_EQ(nack.tick(33), (std::vector<uint16_t>{101}));   // 第 2 次（重试 1）
    EXPECT_EQ(nack.tick(66), (std::vector<uint16_t>{101}));   // 第 3 次（重试 2）
    EXPECT_TRUE(nack.tick(99).empty());       // 已达 3 次上限 → 放弃
    EXPECT_EQ(nack.givenUpCount(), 1u);
    EXPECT_EQ(nack.requestedCount(), 3u);     // 总请求次数 = 3
    EXPECT_TRUE(nack.tick(500).empty());      // 放弃后不再出现
}

TEST(NackRequesterTest, RecoveredPacketNotRequestedAgain) {
    NackRequester nack;
    nack.onMissing({101, 102}, 0);
    nack.onReceived(101);                     // 101 经重传/乱序到达
    auto retry = nack.tick(33);
    EXPECT_EQ(retry, (std::vector<uint16_t>{102}));  // 只重试 102
    EXPECT_EQ(nack.givenUpCount(), 0u);
}

TEST(NackRequesterTest, DuplicateGapNotReRegistered) {
    NackRequester nack;
    nack.onMissing({101}, 0);
    auto again = nack.onMissing({101}, 5);    // 同一 seq 重复上报
    EXPECT_TRUE(again.empty());               // 已在表中，不重复立即请求
    EXPECT_EQ(nack.requestedCount(), 1u);
}

TEST(NackRequesterTest, MixedTimelineWorks) {
    NackRequester nack;
    nack.onMissing({200, 201, 202}, 0);
    nack.onReceived(201);
    auto r1 = nack.tick(34);
    EXPECT_EQ(r1, (std::vector<uint16_t>{200, 202}));
    nack.onReceived(200);
    auto r2 = nack.tick(68);
    EXPECT_EQ(r2, (std::vector<uint16_t>{202}));
    nack.onReceived(202);                     // 全部恢复，无放弃
    EXPECT_TRUE(nack.tick(102).empty());
    EXPECT_EQ(nack.givenUpCount(), 0u);
}
