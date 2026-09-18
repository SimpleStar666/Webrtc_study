// ============================================================================
// spsc_ring_test.cpp — SPSC 无锁环形缓冲测试（工程化升级 v2 Phase D 新增）
// ============================================================================
//
// 本测试文件验证 SpscRing<T> 的正确性，覆盖无锁环形缓冲的三类核心保证：
//
// 1. 数据正确性：FIFO 顺序、跨界回绕不串位、移动语义不深拷贝
// 2. 溢出策略：写侧有界写入丢最新（计数）、消费侧 drop 丢最旧（计数）
// 3. 并发正确性：两线程压测"零丢失 + 零重复"——这是无锁结构最硬
//    的正确性标准：任何撕裂/丢失/重复都会在收到的值序列上暴露
//
// 【测试为什么这样设计】
// - 单线程往返测的是"账本对不对"（计数器与槽位一致）
// - 回绕测试测的是"取模定位"在跨越容量边界多次后依然正确
// - 压测用的是"全量集合比对"而非抽样：SPSC 的正确性是全序性质，
//   收到的第 i 个元素必须恰好等于发送的第 i 个元素，错一个都算输
//
// 【与内存序的关系】
// 压测在 x86（强内存序）上即使实现错误也可能碰巧通过——真正的考验
// 在 ARM。release-acquire 配对写对了，两个平台都稳；这里能在任一
// 平台上把逻辑错误（丢/重/错序）筛出来。
// ============================================================================

#include <gtest/gtest.h>
#include "utils/spsc_ring.h"

#include <thread>
#include <vector>

// ----------------------------------------------------------------------------
// 测试：单线程批量 push/pop 往返
// ----------------------------------------------------------------------------
// FIFO 语义的基本验证：先入先出，读出的顺序与写入的顺序一致。
// 这是最基础的一致性检查——计数器与槽位映射如果有任何错误，
// 都会在这里以乱序/错值的形式暴露。
TEST(SpscRingTest, SingleThreadPushPopRoundtrip) {
    crystal::SpscRing<int> ring(8);

    int items[5] = {1, 2, 3, 4, 5};
    EXPECT_EQ(ring.push(items, 5), 5u);   // 全部写入
    EXPECT_EQ(ring.size(), 5u);

    int out[5] = {0};
    EXPECT_EQ(ring.pop(out, 5), 5u);     // 全部读出
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(out[i], items[i]) << "FIFO 顺序被破坏，下标 " << i;
    }
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.pushDropCount(), 0u);
    EXPECT_EQ(ring.consumerDropCount(), 0u);
}

// ----------------------------------------------------------------------------
// 测试：pushOne/popOne 的移动语义
// ----------------------------------------------------------------------------
// 大对象（视频帧 vector）跨线程传递必须走移动而非拷贝：
// 容量 100 的 vector 每帧拷贝一次 = 白白多一次堆分配 + memcpy。
// 移动后源对象被掏空（size==0），数据所有权完整转移到队列。
TEST(SpscRingTest, PushOnePopOneMoveSemantics) {
    crystal::SpscRing<std::vector<int>> ring(2);

    std::vector<int> frame(100, 42);
    EXPECT_TRUE(ring.pushOne(std::move(frame)));
    EXPECT_TRUE(frame.empty());          // 已被移动掏空，无深拷贝发生

    std::vector<int> out;
    EXPECT_TRUE(ring.popOne(out));
    EXPECT_EQ(out.size(), 100u);
    EXPECT_EQ(out.front(), 42);
    EXPECT_EQ(out.back(), 42);

    // 空队列 popOne 返回 false，不产生任何数据
    EXPECT_FALSE(ring.popOne(out));
}

// ----------------------------------------------------------------------------
// 测试：写侧溢出——有界写入，丢最新并计数
// ----------------------------------------------------------------------------
// 写方的安全契约：只写空闲槽位，绝不覆盖未读数据。
// 放不下的"最新"部分被丢弃（pushDropCount）——丢弃是最坏情况的
// 背压策略，但比阻塞写方或撕裂读方数据都好。
TEST(SpscRingTest, OverflowPushDropsNewest) {
    crystal::SpscRing<int> ring(4);

    int items[6] = {1, 2, 3, 4, 5, 6};   // 容量 4，放 6 个
    EXPECT_EQ(ring.push(items, 6), 4u); // 只写入前 4 个
    EXPECT_EQ(ring.pushDropCount(), 2u); // 5、6 被丢弃并计数

    int out[4] = {0};
    EXPECT_EQ(ring.pop(out, 4), 4u);
    // 未读数据完好无损——先入的 4 个值原样读出
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 3);
    EXPECT_EQ(out[3], 4);
}

// ----------------------------------------------------------------------------
// 测试：消费侧 drop——丢最旧（水位回落/取最新帧的基石）
// ----------------------------------------------------------------------------
// 丢最旧必须由消费者执行（写方动读指针会破坏无锁正确性，见
// spsc_ring.h 头注释）。drop(n) 丢弃 n 个最旧元素并计数。
TEST(SpscRingTest, ConsumerDropOldest) {
    crystal::SpscRing<int> ring(8);

    int items[5] = {1, 2, 3, 4, 5};
    ring.push(items, 5);

    EXPECT_EQ(ring.drop(2), 2u);        // 丢最旧两个（1、2）
    EXPECT_EQ(ring.consumerDropCount(), 2u);

    int out[3] = {0};
    EXPECT_EQ(ring.pop(out, 3), 3u);
    EXPECT_EQ(out[0], 3);               // 读出的从 3 开始
    EXPECT_EQ(out[2], 5);

    EXPECT_EQ(ring.drop(100), 0u);       // 空了再丢，安全返回 0
}

// ----------------------------------------------------------------------------
// 测试：反复跨越容量边界（回绕）
// ----------------------------------------------------------------------------
// 槽位定位 = 计数器 % capacity。生产-消费交替 10 轮、每轮跨越
// 一次容量为 4 的边界，验证模运算定位永不串位。
TEST(SpscRingTest, WraparoundAcrossCapacityBoundary) {
    crystal::SpscRing<int> ring(4);

    for (int round = 0; round < 10; ++round) {
        int v = round * 10 + 7;
        ASSERT_EQ(ring.push(&v, 1), 1u);
        int out = -1;
        ASSERT_EQ(ring.pop(&out, 1), 1u);
        ASSERT_EQ(out, v) << "回绕后槽位串位，轮次 " << round;
    }
    EXPECT_TRUE(ring.empty());
}

// ----------------------------------------------------------------------------
// 测试：两线程压测——零丢失、零重复、零错序
// ----------------------------------------------------------------------------
// 无锁结构最硬的正确性标准：单生产者推 0..N-1，单消费者收满 N 个，
// 收到的序列必须与发送的序列逐位相等。
//   丢失   → 某个值永远不会出现，消费者卡死（测试超时/长度不等）
//   重复   → 某个值出现两次，序列错位
//   撕裂   → 值变成垃圾/半新半旧
//   错序   → 严格递增性质被破坏
// 生产者满时自旋 yield 重试（压测要求全量送达，不走丢弃路径）。
TEST(SpscRingTest, TwoThreadStressNoLossNoDuplication) {
    const uint64_t kTotal = 200000;
    crystal::SpscRing<uint64_t> ring(1024);

    std::thread producer([&] {
        for (uint64_t i = 0; i < kTotal; ++i) {
            while (!ring.pushOne(std::move(i))) {
                std::this_thread::yield();   // 满则让出 CPU 等消费者
            }
        }
    });

    std::vector<uint64_t> received;
    received.reserve(static_cast<size_t>(kTotal));
    std::thread consumer([&] {
        uint64_t v;
        while (received.size() < kTotal) {
            if (ring.popOne(v)) {
                received.push_back(v);
            } else {
                std::this_thread::yield();   // 空则让出 CPU 等生产者
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kTotal);
    for (uint64_t i = 0; i < kTotal; ++i) {
        ASSERT_EQ(received[static_cast<size_t>(i)], i)
            << "位置 " << i << " 的值错误（丢失/重复/撕裂/错序）";
    }
    // 注意：这里不断言 pushDropCount==0——生产者满时自旋重试，
    // 计数器记的是"被拒的尝试次数"（背压次数），不是丢失的元素；
    // 全量零丢失已由上面的逐位比对严格保证。
}
