#pragma once
// 跨线程安全的 rendezvous(unbuffered,容量 0)通道。
//
// 与 boost::fibers::unbuffered_channel 的区别:push/pop 用 std::mutex + std::condition_variable
// 阻塞 **OS 线程**,而不是 active_ctx->suspend() 挂起 fiber。因此可在【任意上下文】调用 ——
// 包括非 fiber 线程(普通 std::thread、跑 Qt 事件循环的主线程的槽里),不依赖 fiber 调度器被驱动。
//
// 实现参照 boost.fiber 的 future shared_state(mtx + cv + ready 标志 + 值存储),把"一次性"的
// future 推广为"可复用"的 rendezvous 交接。
//
// 语义(与 unbuffered_channel 对齐):
//   * 容量 0:push 放下值后【阻塞直到被某个 pop 取走】(rendezvous),再返回 success;
//   * pop 阻塞直到有值;取走后唤醒对应 push;
//   * close():其后正在/后续的 push 返回 closed;pop 在无值时返回 closed。幂等。
//
// 适用:生产者/消费者在【不同 OS 线程】。⚠️ 若在【同一线程的 fiber 之间】用它做交接,阻塞 OS 线程
// 会冻结整个 fiber 调度器 → 死锁;那种同线程 fiber↔fiber 场景仍应用 boost 的 fiber channel。
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>

namespace coro {

enum class channel_status { success, closed };

template <class T>
class sync_channel {
public:
    sync_channel() = default;
    sync_channel(const sync_channel&) = delete;
    sync_channel& operator=(const sync_channel&) = delete;

    // 生产者:放下值并阻塞至被取走(rendezvous)。通道关闭返回 closed。
    channel_status push(T value) {
        std::unique_lock<std::mutex> lk{ mtx_ };
        // 等槽空(上一个值已被取走)或关闭
        slot_free_.wait(lk, [this] { return !has_value_ || closed_; });
        if (closed_) return channel_status::closed;
        slot_ = std::move(value);
        has_value_ = true;
        const unsigned long long my = ++push_seq_;   // 本次 push 的序号
        slot_filled_.notify_one();                   // 唤醒一个消费者
        // 等"本次的值"被取走(taken_seq_ 追上 my)或关闭
        consumed_.wait(lk, [this, my] { return taken_seq_ >= my || closed_; });
        if (taken_seq_ >= my) return channel_status::success;
        // 关闭且未被取走:撤回自己的值,让位给后续
        if (has_value_) {
            has_value_ = false;
            slot_.reset();
            slot_free_.notify_one();
        }
        return channel_status::closed;
    }

    // 消费者:阻塞取一个值。可在【非 fiber】线程调用。无值且已关闭返回 closed。
    channel_status pop(T& out) {
        std::unique_lock<std::mutex> lk{ mtx_ };
        slot_filled_.wait(lk, [this] { return has_value_ || closed_; });
        if (!has_value_) return channel_status::closed;   // 关闭且空
        out = std::move(*slot_);
        slot_.reset();
        has_value_ = false;
        taken_seq_ = push_seq_;        // 告知对应 push:其值已被取走
        consumed_.notify_all();        // 唤醒等待"被取走"的 push
        slot_free_.notify_one();       // 唤醒等待"槽空"的下一个 push
        return channel_status::success;
    }

    void close() {
        std::unique_lock<std::mutex> lk{ mtx_ };
        closed_ = true;
        slot_filled_.notify_all();
        slot_free_.notify_all();
        consumed_.notify_all();
    }

    bool is_closed() const {
        std::unique_lock<std::mutex> lk{ mtx_ };
        return closed_;
    }

private:
    mutable std::mutex      mtx_{};
    std::condition_variable slot_filled_{};   // 有值可取
    std::condition_variable slot_free_{};     // 槽空,可放
    std::condition_variable consumed_{};      // 本次值已被取走
    std::optional<T>        slot_{};
    bool                    has_value_{ false };
    bool                    closed_{ false };
    unsigned long long      push_seq_{ 0 };   // 已放入的 push 序号
    unsigned long long      taken_seq_{ 0 };  // 已取走的最大 push 序号
};

} // namespace coro
