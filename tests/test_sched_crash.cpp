// 复现:自定义 fiber 调度器,在 suspend_until() 里 pump Qt 事件循环。
// 槽函数在这个 eventloop 里执行,槽里调用 awaitable::resolve()(= unbuffered_channel::push)。
// push 会 suspend 调用它的上下文 —— 而此刻"调用它的上下文"是 dispatcher 上下文
// (suspend_until 跑在 dispatcher 里)。挂起 dispatcher 会破坏 thread_local active_ →
// context_initializer::active_ == nullptr → crash。
//
//   bad   : suspend_until 里直接 processEvents(复现 crash)
//   good  : suspend_until 只等待/返回,事件由普通 fiber 泵(对照,正常)
#include <QCoreApplication>
#include <QTimer>
#include <boost/fiber/all.hpp>
#include <boost/fiber/scheduler.hpp>
#include <coro/awaitable.h>
#include <coro/qt_scheduler.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <chrono>
#include <thread>

namespace bf = boost::fibers;

static bool g_pump_in_suspend = false;   // true = bad(在 suspend_until 里 pump)

// 一个最小 round-robin 调度器。区别只在 suspend_until。
class qt_sched : public bf::algo::algorithm {
    bf::scheduler::ready_queue_type rqueue_{};
public:
    void awakened(bf::context* ctx) noexcept override { ctx->ready_link(rqueue_); }
    bf::context* pick_next() noexcept override {
        if (rqueue_.empty()) return nullptr;
        bf::context* c = &rqueue_.front();
        rqueue_.pop_front();
        return c;
    }
    bool has_ready_fibers() const noexcept override { return !rqueue_.empty(); }

    void suspend_until(std::chrono::steady_clock::time_point const&) noexcept override {
        if (g_pump_in_suspend) {
            // —— 用户的写法:在 dispatcher 上下文里 pump Qt 事件 ——
            // 槽函数(含 resolve()→push()→suspend)就在这里被同步执行。
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        } else {
            // 对照:不在这里 pump,只是短暂返回让 dispatcher 重新调度。
            QThread_msleep_noop();
        }
    }
    void notify() noexcept override {}

    static void QThread_msleep_noop() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const char* mode = argc > 1 ? argv[1] : "bad";
    // bad  : 自定义 qt_sched,在 suspend_until 里 pump 事件(复现崩溃)
    // good : 自定义 qt_sched,但事件在普通 fiber 里 pump(对照)
    // safe : 用库提供的 coro::qt_scheduler,事件在普通 fiber 里 pump(正式方案)
    const bool use_safe = (std::strcmp(mode, "safe") == 0);
    g_pump_in_suspend   = (std::strcmp(mode, "bad") == 0);

    if (use_safe) bf::use_scheduling_algorithm<coro::qt_scheduler>();
    else          bf::use_scheduling_algorithm<qt_sched>();

    auto aw = std::make_shared<coro::awaitable<int>>();
    int got = -1;
    bool done = false;

    // 消费者 fiber:park 在 await()。
    bf::fiber([aw, &got, &done]{
        auto r = aw->await();
        got = r.has_value() ? r.value() : -999;
        done = true;
    }).detach();

    const bool dbl = (argc > 2 && !std::strcmp(argv[2], "double"));
    // 定时器:槽里 resolve()。在 bad 模式下,这个槽会在 suspend_until 的 processEvents 里跑。
    QTimer::singleShot(30, [aw, dbl]{
        std::printf("[slot] resolve #1 ...\n"); std::fflush(stdout);
        bool ok = aw->resolve(42);                 // consumer 已 park → 正常 rendezvous
        std::printf("[slot] resolve #1 ok=%d\n", ok); std::fflush(stdout);
        if (dbl) {
            std::printf("[slot] resolve #2 (无 consumer) ...\n"); std::fflush(stdout);
            aw->resolve(43);                        // 无 consumer → push 挂起 dispatcher
            std::printf("[slot] resolve #2 returned(没崩)\n"); std::fflush(stdout);
        }
    });

    // good 模式需要一个普通 fiber 来泵事件(因为 suspend_until 不再 pump)。
    if (!g_pump_in_suspend) {
        bf::fiber([&done]{
            while (!done) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                boost::this_fiber::yield();
            }
        }).detach();
    }

    // 主 fiber:等消费者完成。
    for (int i = 0; i < 2000 && !done; ++i) {
        boost::this_fiber::sleep_for(std::chrono::milliseconds(1));
    }
    // 清理:唤醒可能挂起的(无 consumer)生产者,让 pump fiber 退出,避免退出期悬挂 fiber。
    aw->close();
    for (int i = 0; i < 50; ++i) boost::this_fiber::yield();
    std::printf("[main] done=%d got=%d (期望 42)\n", done, got);
    return (done && got == 42) ? 0 : 1;
}
