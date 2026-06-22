#pragma once
// 安全的 Qt 集成 fiber 调度器。
//
// 关键约束(踩坑根因):boost.fiber 的 algorithm 回调 —— 尤其 suspend_until() ——
// 运行在 *dispatcher 上下文* 里。dispatcher 上下文 **绝不能被挂起**。因此
// suspend_until() 内 **不得** 执行任何会触发 fiber 挂起的代码,包括
// QCoreApplication::processEvents()(它会同步派发 Qt 槽,而槽里常会调用
// awaitable::resolve() → unbuffered_channel::push() → 挂起当前上下文 →
// 挂起 dispatcher → context_initializer::active_ 置空 → 段错误)。
//
// 正确做法:
//   * suspend_until() 只在条件变量上阻塞等待(高效空闲),收到 notify()/超时即返回;
//     它不 pump 事件、不跑槽、不挂起任何 fiber。
//   * Qt 事件由一个 **普通 fiber**(见 coro::exec_qt())pump,槽函数因此在可挂起的
//     普通上下文里运行,resolve() 在那里挂起是合法的。
//   * notify() 同时唤醒 Qt 事件循环(wakeUp),让跨线程唤醒的 fiber 能被及时调度。
#include <boost/fiber/algo/algorithm.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/scheduler.hpp>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <QCoreApplication>
#include <QAbstractEventDispatcher>

namespace coro {

class qt_scheduler : public boost::fibers::algo::algorithm {
public:
    qt_scheduler() = default;
    qt_scheduler(const qt_scheduler&) = delete;
    qt_scheduler& operator=(const qt_scheduler&) = delete;

    void awakened(boost::fibers::context* ctx) noexcept override {
        ctx->ready_link(rqueue_);
    }

    boost::fibers::context* pick_next() noexcept override {
        if (rqueue_.empty()) return nullptr;
        boost::fibers::context* ctx = &rqueue_.front();
        rqueue_.pop_front();
        return ctx;
    }

    bool has_ready_fibers() const noexcept override {
        return !rqueue_.empty();
    }

    // 只阻塞等待 —— 不 pump 事件、不挂起 fiber(否则就会挂起 dispatcher 导致崩溃)。
    void suspend_until(std::chrono::steady_clock::time_point const& tp) noexcept override {
        std::unique_lock<std::mutex> lk{ mtx_ };
        if ((std::chrono::steady_clock::time_point::max)() == tp) {
            cnd_.wait(lk, [this]{ return flag_; });
        } else {
            cnd_.wait_until(lk, tp, [this]{ return flag_; });
        }
        flag_ = false;
    }

    void notify() noexcept override {
        {
            std::unique_lock<std::mutex> lk{ mtx_ };
            flag_ = true;
        }
        cnd_.notify_all();
        // 跨线程把某 fiber 标记为就绪时,顺带唤醒 Qt 事件循环,
        // 让正阻塞在 processEvents(WaitForMoreEvents) 的 pump fiber 立刻返回去调度它。
        if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
    }

private:
    boost::fibers::scheduler::ready_queue_type rqueue_{};
    std::mutex                                 mtx_{};
    std::condition_variable                    cnd_{};
    bool                                       flag_{ false };
};

} // namespace coro
