#include <gtest/gtest.h>
#include "base/lock_free_queue.h"

#include <thread>
#include <vector>
#include <atomic>

using namespace xrtc;

class LockFreeQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }

    LockFreeQueue<int> queue;
};

// ============================================================
// 基本功能测试
// ============================================================

TEST_F(LockFreeQueueTest, CreateAndDestroy) {
    LockFreeQueue<int> queue;
    SUCCEED();
}

TEST_F(LockFreeQueueTest, EmptyQueueConsumeReturnFalse) {
    // 空队列 consume 应返回 false
    int value = -1;
    EXPECT_FALSE(queue.consume(&value));
    // result 不应被修改
    EXPECT_EQ(value, -1);
}

TEST_F(LockFreeQueueTest, EmptyQueueEmptyReturnTrue) {
    // 空队列 empty() 应返回 true
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0);
}

TEST_F(LockFreeQueueTest, ProduceAndConsumeOneElement) {
    queue.produce(42);

    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size(), 1);

    int value = 0;
    EXPECT_TRUE(queue.consume(&value));
    EXPECT_EQ(value, 42);

    // 消费后队列应为空
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0);
}

TEST_F(LockFreeQueueTest, FifoOrder) {
    // 验证先进先出顺序
    const int n = 100;
    for (int i = 0; i < n; ++i) {
        queue.produce(i);
    }

    EXPECT_EQ(queue.size(), n);

    for (int i = 0; i < n; ++i) {
        int value = -1;
        EXPECT_TRUE(queue.consume(&value));
        EXPECT_EQ(value, i);
    }

    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0);
}

TEST_F(LockFreeQueueTest, ProduceAndConsumeAlternating) {
    // 交替生产消费
    for (int i = 0; i < 10; ++i) {
        queue.produce(i);
        int value = -1;
        EXPECT_TRUE(queue.consume(&value));
        EXPECT_EQ(value, i);
    }
    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, LargeNumberOfElements) {
    // 大量元素测试
    const int n = 10000;
    for (int i = 0; i < n; ++i) {
        queue.produce(i);
    }

    EXPECT_EQ(queue.size(), n);

    for (int i = 0; i < n; ++i) {
        int value = -1;
        EXPECT_TRUE(queue.consume(&value));
        EXPECT_EQ(value, i);
    }

    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, ConsumeAllThenProduceAgain) {
    // 消费完所有元素后再次生产消费
    queue.produce(1);
    queue.produce(2);
    int value;
    queue.consume(&value);
    queue.consume(&value);

    EXPECT_TRUE(queue.empty());

    queue.produce(3);
    queue.produce(4);
    queue.produce(5);

    EXPECT_EQ(queue.size(), 3);

    EXPECT_TRUE(queue.consume(&value));
    EXPECT_EQ(value, 3);
    EXPECT_TRUE(queue.consume(&value));
    EXPECT_EQ(value, 4);
    EXPECT_TRUE(queue.consume(&value));
    EXPECT_EQ(value, 5);

    EXPECT_TRUE(queue.empty());
}

// ============================================================
// 多线程测试
// ============================================================

TEST_F(LockFreeQueueTest, SingleProducerSingleConsumer) {
    // 单生产者-单消费者多线程测试
    const int n = 100000;
    std::atomic<int> consume_count{0};

    std::thread producer([this, n]() {
        for (int i = 0; i < n; ++i) {
            queue.produce(i);
        }
    });

    std::thread consumer([this, n, &consume_count]() {
        int last_value = -1;
        int count = 0;
        while (count < n) {
            int value;
            if (queue.consume(&value)) {
                // 验证 FIFO 顺序
                EXPECT_EQ(value, last_value + 1);
                last_value = value;
                ++count;
            }
        }
        consume_count = count;
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(consume_count, n);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size(), 0);
}

TEST_F(LockFreeQueueTest, ProducerConsumerInterleaved) {
    // 生产者生产一批后消费者消费一批，交替进行
    const int batch_size = 1000;
    const int rounds = 10;

    std::thread producer([this, batch_size, rounds]() {
        for (int r = 0; r < rounds; ++r) {
            int base = r * batch_size;
            for (int i = 0; i < batch_size; ++i) {
                queue.produce(base + i);
            }
            // 让消费者有机会消费
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    std::thread consumer([this, batch_size, rounds]() {
        int expected = 0;
        int total = batch_size * rounds;
        int count = 0;
        while (count < total) {
            int value;
            if (queue.consume(&value)) {
                EXPECT_EQ(value, expected);
                ++expected;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, ConsumerFasterThanProducer) {
    // 消费者快于生产者：消费者经常遇到空队列
    const int n = 10000;

    std::thread producer([this, n]() {
        for (int i = 0; i < n; ++i) {
            queue.produce(i);
            // 慢速生产，让消费者经常遇到空队列
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    std::thread consumer([this, n]() {
        int expected = 0;
        int count = 0;
        while (count < n) {
            int value;
            if (queue.consume(&value)) {
                EXPECT_EQ(value, expected);
                ++expected;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.empty());
}

TEST_F(LockFreeQueueTest, ProducerFasterThanConsumer) {
    // 生产者快于消费者：队列中会积累大量元素
    const int n = 100000;

    std::thread producer([this, n]() {
        for (int i = 0; i < n; ++i) {
            queue.produce(i);
        }
    });

    std::thread consumer([this, n]() {
        int expected = 0;
        int count = 0;
        while (count < n) {
            int value;
            if (queue.consume(&value)) {
                EXPECT_EQ(value, expected);
                ++expected;
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.empty());
}

// ============================================================
// 自定义类型测试
// ============================================================

struct TestData {
    int id;
    double value;
    std::string name;

    TestData() : id(0), value(0.0), name("") {}
    TestData(int id, double value, const std::string& name)
        : id(id), value(value), name(name) {}
};

TEST(LockFreeQueueCustomTypeTest, CustomStructType) {
    LockFreeQueue<TestData> queue;

    queue.produce(TestData(1, 3.14, "pi"));
    queue.produce(TestData(2, 2.718, "e"));

    EXPECT_EQ(queue.size(), 2);

    TestData data;
    EXPECT_TRUE(queue.consume(&data));
    EXPECT_EQ(data.id, 1);
    EXPECT_DOUBLE_EQ(data.value, 3.14);
    EXPECT_EQ(data.name, "pi");

    EXPECT_TRUE(queue.consume(&data));
    EXPECT_EQ(data.id, 2);
    EXPECT_DOUBLE_EQ(data.value, 2.718);
    EXPECT_EQ(data.name, "e");

    EXPECT_TRUE(queue.empty());
}

TEST(LockFreeQueueCustomTypeTest, CustomTypeMultiThread) {
    LockFreeQueue<TestData> queue;
    const int n = 10000;

    std::thread producer([&queue, n]() {
        for (int i = 0; i < n; ++i) {
            queue.produce(TestData(i, i * 1.5, "data_" + std::to_string(i)));
        }
    });

    std::thread consumer([&queue, n]() {
        int count = 0;
        while (count < n) {
            TestData data;
            if (queue.consume(&data)) {
                EXPECT_EQ(data.id, count);
                EXPECT_DOUBLE_EQ(data.value, count * 1.5);
                EXPECT_EQ(data.name, "data_" + std::to_string(count));
                ++count;
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_TRUE(queue.empty());
}
