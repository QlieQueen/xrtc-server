/**
 * @file lock_free_queue.h
 * @brief 单生产者-单消费者（SPSC）无锁队列
 * 
 * 设计目标：
 *   为WebRTC等高性能多媒体框架提供低延迟的线程间数据传递机制。
 *   典型使用场景：编码线程（生产者）-> 网络发送线程（消费者）
 * 
 * 核心设计思路：
 *   1. 基于单向链表，无容量限制，按需动态分配
 *   2. 使用哑结点（dummy node）简化边界条件处理
 *   3. 生产者负责内存清理，消费者不涉及内存管理
 *   4. 除_size外，不依赖原子操作，指针读写天然对齐保证正确性
 * 
 * 线程安全约束（务必遵守）：
 *   - 有且仅有一个生产者线程调用 produce()
 *   - 有且仅有一个消费者线程调用 consume()、empty()、size()
 *   - 违反上述约束将导致数据竞争，没有任何保护机制
 * 
 * 内存模型说明：
 *   - 所有节点在 produce() 中通过 new 分配
 *   - 已消费节点的内存在 produce() 中通过 delete 回收
 *   - 队列销毁时会清理所有残留节点（包括未消费的）
 * 
 * 性能特征：
 *   - produce()/consume() 均为 O(1) 时间复杂度
 *   - 不存在锁竞争，适合高频调用场景
 *   - empty()/size() 返回瞬时值，调用者需接受结果可能立即过时
 * 
 * 已知限制：
 *   - _size 变量可能与指针共享缓存行，存在伪共享风险
 *     优化建议：使用 alignas(64) 让 _size 独占一条缓存行
 *   - 仅支持SPSC，无法扩展到多生产者或多消费者
 *   - 不支持 emplace 构造，元素必须是可拷贝的
 */

#ifndef __LOCK_FREE_QUEUE_H_
#define __LOCK_FREE_QUEUE_H_

#include <atomic>

namespace xrtc {

/**
 * @class LockFreeQueue
 * @brief 线程安全的单生产者-单消费者无锁队列
 * 
 * @tparam T 队列存储的元素类型，要求：
 *           - 可拷贝构造（CopyConstructible）
 *           - 可拷贝赋值（CopyAssignable）
 *           - 可默认构造（用于哑结点）
 * 
 * 使用示例：
 * @code
 *   // 生产者线程
 *   LockFreeQueue<AudioFrame> audio_queue;
 *   audio_queue.produce(frame);
 * 
 *   // 消费者线程
 *   AudioFrame frame;
 *   if (audio_queue.consume(&frame)) {
 *       // 处理frame
 *   }
 * @endcode
 */
template <typename T>
class LockFreeQueue {
private:
    /**
     * @struct Node
     * @brief 队列的链表节点
     * 
     * 每个节点包含一个数据元素和指向下一个节点的指针。
     * 节点内存在 produce() 中分配，在 produce() 的清理循环中释放。
     */
    struct Node {
        T value;        ///< 存储的数据元素
        Node* next;     ///< 指向链表中的下一个节点

        /**
         * @brief 构造节点，初始化数据和next指针
         * @param value 要存储的数据（通过拷贝构造传入）
         */
        Node(const T& value) : value(value), next(nullptr) {} 
    };

    /**
     * 三个核心指针的语义说明：
     * 
     * 链表逻辑结构：
     *   [first] -> [Node(dummy)] -> [Node(1)] -> [Node(2)] -> ... -> [Node(N)] <- [last]
     *                             ^                                          ^
     *                             |                                          |
     *                         [divider]                                  [last]
     * 
     * - first:  链表物理头节点指针。始终指向哑结点或更早的已释放前的位置
     *           （仅在 produce() 清理循环中移动）
     * - divider: 消费分隔线。divider 自身及其左侧节点已被消费，
     *            divider->next 是下一个可消费节点
     * - last:    链表尾节点指针。新元素总是追加到 last->next
     * 
     * 初始化状态：
     *   first == divider == last (都指向同一个哑结点)
     *   此时队列为空，哑结点的 value 无意义
     * 
     * 非空状态示例（3个元素）：
     *   first -> [dummy] -> [A] -> [B] -> [C] <- last
     *                            ^
     *                            |
     *                        [divider]  (A已被消费，B是下一个可消费元素)
     */
    Node* first;        ///< 链表物理头指针（用于内存回收）
    Node* divider;      ///< 消费分隔线（divider->next是第一个可消费节点）
    Node* last;         ///< 链表尾指针（新元素追加位置）
    
    /**
     * @brief 队列当前元素数量
     * 
     * 使用 std::atomic<int> 保证多线程环境下的基本可见性。
     * 注意：empty() 和 size() 返回的是瞬时快照值，
     * 在高并发场景下，返回值可能在读取后立即改变。
     * 
     * 性能优化提示：
     *   此变量可能与上述三个指针共享同一CPU缓存行（通常64字节），
     *   导致生产者++_size和消费者--_size时产生缓存行乒乓效应。
     *   如性能剖析显示此处为热点，可改为：
     *   @code
     *   alignas(64) std::atomic<int> _size;
     *   @endcode
     */
    std::atomic<int> _size;

public:    
    /**
     * @brief 构造函数，创建空队列
     * 
     * 初始化时创建哑结点，三个指针均指向它。
     * 哑结点的作用是：
     *   1. 避免空队列时 first/divider/last 为 nullptr
     *   2. 简化 consume() 中的非空判断：只需检查 divider != last
     *   3. 使 produce() 中的清理循环有明确的终止条件
     */
    LockFreeQueue() {
        first = divider = last = new Node(T());
        _size = 0;
    }

    /**
     * @brief 析构函数，清理所有节点
     * 
     * 从 first 开始遍历整个链表并释放所有节点内存，
     * 包括哑结点和所有未被消费的数据节点。
     * 
     * 安全警告：
     *   调用此析构函数前，必须确保生产者/消费者线程已完全停止。
     *   否则将导致 use-after-free（使用已释放的内存），引发未定义行为。
     */
    ~LockFreeQueue() {
        while (first != nullptr) {
            Node* temp = first;
            first = first->next;
            delete temp;
        }

        _size = 0;
    }

    /**
     * @brief 生产者线程接口：向队列尾部添加元素
     * 
     * 执行流程：
     *   1. 在 last->next 创建新节点（追加到链表尾部）
     *   2. 更新 last 指针指向新节点
     *   3. 原子递增 _size 计数器
     *   4. 执行延迟清理：回收所有已被消费的节点
     * 
     * 延迟清理机制说明：
     *   - 消费者消费后只移动 divider，不释放内存
     *   - 生产者在下一次 produce() 调用时负责清理
     *   - 清理条件：divider != first（有已消费节点可回收）
     *   - 清理范围：从 first 开始，到 divider 为止（不含divider自身）
     *   - 如果队列为空（divider == first），跳过清理，无额外开销
     * 
     * 这种设计的好处：
     *   1. 消费者路径极短，延迟更低
     *   2. 内存管理集中在生产者一侧，简化了线程同步
     *   3. 避免了消费者释放内存后生产者仍持有指针的悬垂指针问题
     * 
     * 线程安全：仅限单个生产者线程调用
     * 
     * @param t 要添加到队列尾部的元素（通过const引用传入，内部会拷贝）
     */
    void produce(const T& t) {
        // 步骤1-2：向链表尾部追加新节点
        last->next = new Node(t);    
        last = last->next;
        
        // 步骤3：更新元素计数（原子操作，消费者可见）
        ++_size;
        
        // 步骤4：延迟清理已消费节点
        // 注意：清理工作由生产者负责，消费者只管取数据不管释放
        while (divider != first) {
            Node* temp = first;
            first = first->next;
            delete temp;
        }
    }

    /**
     * @brief 消费者线程接口：从队列头部取出一个元素
     * 
     * 执行流程：
     *   1. 检查队列是否非空：divider != last
     *   2. 从 divider->next 取出元素值（拷贝给调用者）
     *   3. 向前移动 divider（该节点变为"已消费"状态）
     *   4. 原子递减 _size 计数器
     * 
     * 非空判断原理：
     *   - 空队列：divider == last（都指向哑结点或同一个节点）
     *   - 非空队列：divider 落后于 last（divider->next 有数据）
     *   - 哑结点保证首次进入时 divider != last 恰当地表示空状态
     * 
     * 线程安全：仅限单个消费者线程调用
     * 
     * @param result 输出参数，用于接收取出的元素值
     *               注意：通过指针传递，调用者需确保指针非空
     * @return true  成功取出元素，*result 包含元素值
     * @return false 队列为空，*result 未被修改
     */
    bool consume(T* result) {
        // 检查队列是否非空
        if (divider != last) {
            // 从 divider->next 取出元素值
            *result = divider->next->value;
            
            // 移动 divider，标记该节点为"已消费"
            divider = divider->next;
            
            // 更新元素计数
            --_size;
            return true;
        }

        return false;
    }

    /**
     * @brief 检查队列是否为空
     * 
     * 通过原子变量 _size 判断，避免直接访问指针。
     * 
     * 瞬时性说明：
     *   返回值为调用时刻的快照。在高并发场景下：
     *   - 返回 true 可能立即变为 false（生产者刚好添加了元素）
     *   - 返回 false 可能立即变为 true（消费者刚好取走了最后一个元素）
     *   调用者应将此返回值视为"大概率准确"而非"绝对保证"。
     * 
     * 线程安全：仅限单个消费者线程调用
     * 
     * @return true  队列当前为空（快照值）
     * @return false 队列当前非空（快照值）
     */
    bool empty() {
        return _size == 0;
    }

    /**
     * @brief 获取队列中当前的元素数量
     * 
     * 与 empty() 相同的瞬时性特征。
     * 
     * 线程安全：仅限单个消费者线程调用
     * 
     * @return 队列中的元素数量（快照值）
     */
    int size() const {
        return _size;
    }
};


}


#endif