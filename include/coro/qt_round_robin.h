#pragma once
// coro::qt_round_robin —— 仿 boost.fiber 官方 examples/asio/round_robin.hpp 的 Qt 调度器。
//
// 范式:**事件循环本身驱动 fiber 调度**。主 fiber 经 qt_round_robin::run() 跑 service loop;
// Qt 事件在该 service-loop fiber(可挂起的普通上下文)里 pump。suspend_until() 跑在 dispatcher
// 上下文,**绝不 pump、绝不挂起** —— 它只为 boost.fiber 原生定时等待启动一个 QTimer,并 notify
// 唤醒 service loop。这样槽里的 resolve()(= unbuffered_channel::push 的潜在挂起)发生在
// service-loop fiber 而非 dispatcher,根除了 tests/test_sched_crash.cpp / test_resolve_crash.cpp
// 诊断出的崩溃类。详见 docs/.../2026-06-22-qt-round-robin-design.md。
//
// 用法:
//   QCoreApplication app(argc, argv);
//   bf::use_scheduling_algorithm<coro::qt_round_robin>();
//   bf::fiber([]{ /* sleep/await 工作 */ coro::qt_round_robin::stop(); }).detach();
//   coro::qt_round_robin::run();   // 主 fiber 驱动 service loop 直到 stop()
#include <boost/fiber/algo/algorithm.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/scheduler.hpp>
#include <boost/fiber/mutex.hpp>
#include <boost/fiber/condition_variable.hpp>
#include <boost/fiber/operations.hpp>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QAbstractEventDispatcher>

namespace coro {

class qt_round_robin : public boost::fibers::algo::algorithm {
public:
    qt_round_robin() {
        // 构造发生在调用 use_scheduling_algorithm 的线程(= 调度器线程)。
        // 缓存线程对象而非 dispatcher 指针:boost.fiber 的 thread_local 调度器比
        // QCoreApplication 活得久,退出期 QCoreApplication 先析构,再缓存的 dispatcher 指针
        // 就会悬空(notify() 在调度器收尾时仍可能被调用 → use-after-free)。改为每次按线程
        // 重新查询 dispatcher:线程/应用拆除后查询返回 nullptr,安全跳过。
        thread_ = QThread::currentThread();
        suspend_timer_.setSingleShot(true);
        // 必须 PreciseTimer:默认 Qt::CoarseTimer 可提前至多 5% 触发,会早于 fiber 的真实
        // steady_clock 唤醒时刻,届时 sleep2ready 判定 sleeper 未到期、不唤醒它,而单次 QTimer
        // 已耗尽 → processEvents 永久挂死(timerWakesFiber 间歇性 hang 的根因)。
        suspend_timer_.setTimerType(Qt::PreciseTimer);
        instance_ptr() = this;
    }
    qt_round_robin(const qt_round_robin&) = delete;
    qt_round_robin& operator=(const qt_round_robin&) = delete;

    // 就绪入队;dispatcher 上下文不计入 counter_(与 asio round_robin 一致)。
    void awakened(boost::fibers::context* ctx) noexcept override {
        ctx->ready_link(rqueue_);
        if (!ctx->is_context(boost::fibers::type::dispatcher_context)) ++counter_;
    }

    boost::fibers::context* pick_next() noexcept override {
        if (rqueue_.empty()) return nullptr;
        boost::fibers::context* ctx = &rqueue_.front();
        rqueue_.pop_front();
        if (!ctx->is_context(boost::fibers::type::dispatcher_context)) --counter_;
        return ctx;
    }

    bool has_ready_fibers() const noexcept override { return 0 < counter_; }

    // 修复点:不 pump、不阻塞、不挂起 fiber。只为原生定时等待安排一个 QTimer 唤醒,
    // 再 notify service loop。跑在 dispatcher 上下文 —— 任何挂起都会破坏它。
    void suspend_until(std::chrono::steady_clock::time_point const& tp) noexcept override {
        if ((std::chrono::steady_clock::time_point::max)() != tp) {
            auto now = std::chrono::steady_clock::now();
            // 关键:**向上取整**到毫秒。截断(floor)会让 QTimer 早于 fiber 的真实唤醒时刻
            // 触发,届时 sleep2ready 认为 sleeper 未到期、不唤醒它,而单次 QTimer 已耗尽 →
            // processEvents 永久阻塞挂死。ceil 保证 QTimer 不早于 deadline。
            long long ms = 0;
            if (tp > now) {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp - now).count();
                ms = (ns + 999'999) / 1'000'000;   // ceil 到毫秒
            }
            // 此处就在调度器线程,操作 QTimer 安全。QTimer 到期投递一个事件,
            // 唤醒 service loop 阻塞中的 processEvents(WaitForMoreEvents)。
            suspend_timer_.start(static_cast<int>(std::min<long long>(ms, 1000ll * 60 * 60 * 24)));
        }
        cnd_.notify_one();
    }

    // 跨线程唤醒:仅 wakeUp()(线程安全),让 service loop 的 processEvents 返回去排空 remote-ready。
    void notify() noexcept override {
        if (auto* d = QAbstractEventDispatcher::instance(thread_)) d->wakeUp();
    }

    // service loop —— 跑在主 fiber。安装算法后由主 fiber 调用。
    static void run()  { instance_ptr()->run_();  }
    // 由调度器线程上的 fiber 调用(用到 fiber cv)。
    static void stop() { instance_ptr()->stop_(); }

private:
    void run_() {
        running_ = true;
        while (running_) {
            if (has_ready_fibers()) {
                // 让 dispatcher 跑就绪 worker;它们都挂起后 suspend_until 经 cnd_ 唤醒我。
                std::unique_lock<boost::fibers::mutex> lk{ mtx_ };
                cnd_.wait(lk);
            } else {
                // 空闲:阻塞线程等下一个 Qt 事件(对齐 asio io.run_one())。
                QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents | QEventLoop::AllEvents);
                // 强制一次 dispatcher 轮转,吸收 remote-ready / 到期 sleep 的 fiber。
                boost::this_fiber::yield();
            }
        }
    }

    void stop_() {
        running_ = false;
        cnd_.notify_all();
        if (auto* d = QAbstractEventDispatcher::instance(thread_)) d->wakeUp();
    }

    static qt_round_robin*& instance_ptr() {
        static thread_local qt_round_robin* p = nullptr;
        return p;
    }

    boost::fibers::scheduler::ready_queue_type rqueue_{};
    boost::fibers::mutex                       mtx_{};
    boost::fibers::condition_variable          cnd_{};
    std::size_t                                counter_{ 0 };
    QTimer                                     suspend_timer_{};
    QThread*                                   thread_{ nullptr };
    bool                                       running_{ false };
};

} // namespace coro
