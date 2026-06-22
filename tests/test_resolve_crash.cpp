// 复现:在事件循环线程上,对一个"当前没有 fiber 停在 await() 的" awaitable 调用 resolve(),
// 会挂起事件泵 fiber,调度器无可运行 fiber → context::resume(nullptr) → 崩溃。
//
// 用法:./test_resolve_crash <scenario>
//   control       : 有 fiber 已 park 在 await() 时 resolve() —— 正常(对照组)
//   double        : 同一个 Qt 槽里连续 resolve 两次 —— 第二次无 consumer,复现崩溃
//   no_awaiter    : 根本没有 fiber 在 await,直接在槽里 resolve —— 复现崩溃
//   before_await  : resolve 早于 fiber 真正 park —— 复现崩溃
//
// 不带参数时跑 control(可纳入回归)。
#include <QCoreApplication>
#include <QTimer>
#include <coro/awaitable.h>
#include <coro/core.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

static int g_got = -1;

// 对照组:fiber 先 park 在 await(),再由定时器 resolve —— 与 test_awaitable 的 basicResolveAwait 同构。
static int scenario_control() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    coro::launch([aw]{
        auto r = aw->await();        // park 在这里
        g_got = r.value();
        coro::quit();
    });
    QTimer::singleShot(20, [aw]{ aw->resolve(42); });   // 此刻 fiber 已 park
    coro::exec();
    std::printf("[control] got=%d (期望 42)\n", g_got);
    return g_got == 42 ? 0 : 1;
}

// 复现 A:同一个槽里连续 resolve 两次。
// 第一次:有 consumer 在等 → push 把 worker 排进就绪队列后立即返回(worker 尚未运行)。
// 第二次:consumer_ 已被清空 → push 没有等待者 → 挂起当前(事件泵)fiber → 崩溃。
static int scenario_double() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    coro::launch([aw]{
        auto r = aw->await();        // 只 await 一次,取完值后不再 park
        g_got = r.value();
        coro::quit();
    });
    QTimer::singleShot(20, [aw]{
        std::printf("[double] resolve #1 ...\n"); std::fflush(stdout);
        aw->resolve(1);              // ok:worker 被排入就绪队列(还没跑)
        std::printf("[double] resolve #2 ...\n"); std::fflush(stdout);
        aw->resolve(2);              // 崩溃点:无 consumer,挂起事件泵 fiber
        std::printf("[double] (若看到这行说明没崩)\n"); std::fflush(stdout);
    });
    coro::exec();
    return 0;
}

// 复现 B:没有任何 fiber 在 await,直接在事件循环线程的槽里 resolve()。
static int scenario_no_awaiter() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    QTimer::singleShot(20, [aw]{
        std::printf("[no_awaiter] resolve ...\n"); std::fflush(stdout);
        aw->resolve(7);              // 无 consumer → 挂起事件泵 fiber → 崩溃
        std::printf("[no_awaiter] (若看到这行说明没崩)\n"); std::fflush(stdout);
    });
    QTimer::singleShot(200, []{ coro::quit(); });
    coro::exec();
    return 0;
}

// 复现 C:resolve 早于 fiber 真正 park 到 await()。
static int scenario_before_await() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    // 槽在 fiber 还没机会 park 之前就 resolve。
    QTimer::singleShot(0, [aw]{
        std::printf("[before_await] resolve ...\n"); std::fflush(stdout);
        aw->resolve(9);
        std::printf("[before_await] (若看到这行说明没崩)\n"); std::fflush(stdout);
    });
    coro::launch([aw]{
        // 故意先让出几次,确保槽先于 await 执行
        for (int i = 0; i < 3; ++i) coro::yield();
        auto r = aw->await();
        g_got = r.value();
        coro::quit();
    });
    QTimer::singleShot(200, []{ coro::quit(); });
    coro::exec();
    return 0;
}

// 复现 D:worker fiber 在主线程 park 在 await();一个 std::thread 去 resolve(用户说"不崩")。
static int scenario_stdthread() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    coro::launch([aw]{
        auto r = aw->await();
        g_got = r.value();
        coro::quit();
    });
    std::thread th([aw]{
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 等 worker park 好
        std::printf("[stdthread] resolve from std::thread ...\n"); std::fflush(stdout);
        bool ok = aw->resolve(123);                                  // 跨线程 push
        std::printf("[stdthread] resolve returned ok=%d\n", ok); std::fflush(stdout);
    });
    coro::exec();
    th.join();
    std::printf("[stdthread] got=%d (期望 123)\n", g_got);
    return g_got == 123 ? 0 : 1;
}

// 复现 E:std::thread 在"没有 consumer 在 await"时 resolve —— 看是否挂起/崩溃。
static int scenario_stdthread_no_await() {
    auto aw = std::make_shared<coro::awaitable<int>>();
    std::thread th([aw]{
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::printf("[st_no_await] resolve(no consumer) ...\n"); std::fflush(stdout);
        bool ok = aw->resolve(5);     // 该线程上 push 无 consumer
        std::printf("[st_no_await] resolve returned ok=%d\n", ok); std::fflush(stdout);
    });
    QTimer::singleShot(300, []{ coro::quit(); });
    coro::exec();
    aw->close();                      // 唤醒可能挂起的 producer
    th.join();
    return 0;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const char* s = argc > 1 ? argv[1] : "control";
    if (!std::strcmp(s, "control"))            return scenario_control();
    if (!std::strcmp(s, "double"))             return scenario_double();
    if (!std::strcmp(s, "no_awaiter"))         return scenario_no_awaiter();
    if (!std::strcmp(s, "before_await"))       return scenario_before_await();
    if (!std::strcmp(s, "stdthread"))          return scenario_stdthread();
    if (!std::strcmp(s, "stdthread_no_await")) return scenario_stdthread_no_await();
    std::fprintf(stderr, "unknown scenario: %s\n", s);
    return 2;
}
