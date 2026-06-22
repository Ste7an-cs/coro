#pragma once
// 跨线程安全的【缓冲】通道(无界,std::queue 支撑,可容纳多个元素)。
//
// 与 boost::fibers::buffered_channel 的区别:push/pop 用 std::mutex + std::condition_variable
// 阻塞 **OS 线程**,而不是 active_ctx->suspend() 挂起 fiber。因此可在【任意上下文】调用 ——
// 包括非 fiber 线程(普通 std::thread、跑 Qt 事件循环的主线程的槽里),不依赖 fiber 调度器被驱动。
//
// 实现参照 boost.fiber future 的 shared_state(mtx + cv + 状态),把单值/一次性推广为
// 一条 FIFO 队列的可复用交接。
//
// 语义:
//   * 无界缓冲:push 把值入队后【立即返回 success】(不等待消费,可连续放入多个元素);
//   * pop 阻塞直到队列非空;按 FIFO 取出;
//   * close():其后 push 返回 closed;pop 先把队列里【剩余元素取尽】,队列空后才返回 closed。幂等。
//
// 跨线程安全(多生产者/多消费者均可)。可在非 fiber 线程 push/pop。
// 若需背压(有界、满时 push 阻塞),在 push 里加一个 not_full_ 条件等待即可,本实现暂为无界。
#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

namespace coro {

enum class channel_status { success, closed };

template <class T>
class sync_channel {
public:
    sync_channel() = default;
    sync_channel(const sync_channel&) = delete;
    sync_channel& operator=(const sync_channel&) = delete;

    // 生产者:入队并立即返回(无界,不阻塞)。通道关闭返回 closed。
    channel_status push(T value) {
        std::unique_lock<std::mutex> lk{ mtx_ };
        if (closed_) return channel_status::closed;
        queue_.push(std::move(value));
        not_empty_.notify_one();        // 唤醒一个消费者
        return channel_status::success;
    }

    // 消费者:阻塞取一个值(FIFO)。可在【非 fiber】线程调用。
    // 关闭后仍可取尽剩余元素;队列空且已关闭返回 closed。
    channel_status pop(T& out) {
        std::unique_lock<std::mutex> lk{ mtx_ };
        not_empty_.wait(lk, [this] { return !queue_.empty() || closed_; });
        if (queue_.empty()) return channel_status::closed;   // 关闭且已取尽
        out = std::move(queue_.front());
        queue_.pop();
        return channel_status::success;
    }

    void close() {
        std::unique_lock<std::mutex> lk{ mtx_ };
        closed_ = true;
        not_empty_.notify_all();        // 唤醒所有阻塞的消费者(取尽剩余 / 得到 closed)
    }

    bool is_closed() const {
        std::unique_lock<std::mutex> lk{ mtx_ };
        return closed_;
    }

    bool empty() const {
        std::unique_lock<std::mutex> lk{ mtx_ };
        return queue_.empty();
    }

private:
    mutable std::mutex      mtx_{};
    std::condition_variable not_empty_{};   // 队列非空(或已关闭)
    std::queue<T>           queue_{};
    bool                    closed_{ false };
};

} // namespace coro
