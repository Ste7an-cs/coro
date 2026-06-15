#pragma once
#include <boost/fiber/all.hpp>
#include <QCoreApplication>
#include <QAbstractEventDispatcher>
#include <chrono>
#include <type_traits>
#include <utility>

namespace coro {

namespace detail {
// 唤醒 Qt 事件分发器：单线程“泵-驱动”模型下，驱动协程在 exec() 里阻塞于
// processEvents(WaitForMoreEvents)。当 async 协程完成（设置/重抛结果）时，
// 必须唤醒分发器，使 processEvents 返回，从而把等待结果的协程重新调度起来；
// 否则线程会永久停在 WaitForMoreEvents 上形成死锁。
inline void wakeDriver() {
    if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
}
// RAII：协程函数体退出（正常返回或异常展开）时唤醒驱动。
struct DriverWaker { ~DriverWaker() { wakeDriver(); } };
} // namespace detail

template<class T>
class Task {
public:
    Task() = default;
    explicit Task(boost::fibers::future<T> f) : fut_(std::move(f)) {}
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 等待并取结果（异常重抛）
    T get() { return fut_.get(); }

    // 仅等待完成，不消费结果
    void wait() { fut_.wait(); }

    // 非消费式：是否已就绪
    bool done() const {
        return fut_.valid() &&
               fut_.wait_for(std::chrono::seconds(0)) == boost::fibers::future_status::ready;
    }

    // 非阻塞续接：起一个 watcher fiber，完成后回调（消费本 Task）
    template<class F>
    void then(F cb) {
        boost::fibers::fiber([f = std::move(fut_), cb = std::move(cb)]() mutable {
            detail::DriverWaker waker;
            if constexpr (std::is_void_v<T>) { f.get(); cb(); }
            else                            { cb(f.get()); }
        }).detach();
    }

private:
    boost::fibers::future<T> fut_;
};

// 启动协程并返回句柄
template<class Fn>
auto async(Fn fn) -> Task<std::invoke_result_t<Fn>> {
    using T = std::invoke_result_t<Fn>;
    return Task<T>(boost::fibers::async([fn = std::move(fn)]() mutable -> T {
        detail::DriverWaker waker;   // 完成时唤醒驱动循环，避免泵-驱动模型死锁
        return fn();
    }));
}

// 等另一个 Task 完成取结果
template<class T>
T await(Task<T>& t) { return t.get(); }

} // namespace coro
