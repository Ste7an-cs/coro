// coro::qt_round_robin —— 仿 asio round_robin 的 Qt 事件循环 fiber 调度器。
// 与既有 coro::exec() 不同:主 fiber 经 qt_round_robin::run() 驱动 service loop,
// 事件在 service-loop fiber(可挂起的普通上下文)里 pump,suspend_until 不 pump、不挂起。
//
// 三类关键场景(对应设计文档):
//   (a) QTimer 唤醒推进一个用 boost.fiber **原生**定时等待(sleep_for)挂起的 fiber;
//   (b) 信号 await:槽里 resolve 一个 park 着的 fiber —— 不崩(诊断出的崩溃类消失);
//   (c) std::thread 调用 resolve() 跨线程唤醒 park 着的 fiber(对齐 asio round_robin 语义)。
#include <QtTest>
#include <QTimer>
#include <boost/fiber/all.hpp>
#include <coro/awaitable.h>
#include <coro/qt_round_robin.h>
#include <chrono>
#include <memory>
#include <thread>

namespace bf = boost::fibers;

class TestRoundRobin : public QObject {
    Q_OBJECT
private slots:
    // 调度器算法每线程安装一次;后续每个用例各跑一轮 run()/stop()。
    void initTestCase() {
        bf::use_scheduling_algorithm<coro::qt_round_robin>();
    }

    // (a) 原生 fiber sleep_for 经 suspend_until 的 QTimer 唤醒推进。
    void timerWakesFiber() {
        bool advanced = false;
        bf::fiber([&]{
            boost::this_fiber::sleep_for(std::chrono::milliseconds(30));
            advanced = true;
            coro::qt_round_robin::stop();
        }).detach();
        coro::qt_round_robin::run();
        QVERIFY(advanced);
    }

    // (b) 槽里 resolve 一个 park 着的 fiber —— rendezvous 在 service-loop fiber 上发生,安全。
    void slotResolveNoCrash() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        int got = -1;
        bf::fiber([&, aw]{
            auto r = aw->await();
            got = r.value();
            coro::qt_round_robin::stop();
        }).detach();
        QTimer::singleShot(20, [aw]{ aw->resolve(42); });
        coro::qt_round_robin::run();
        QCOMPARE(got, 42);
    }

    // (c) 跨线程 resolve:notify() → dispatcher->wakeUp() 让 service loop 排空 remote-ready。
    void crossThreadResolve() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        int got = -1;
        bf::fiber([&, aw]{
            auto r = aw->await();
            got = r.value();
            coro::qt_round_robin::stop();
        }).detach();
        std::thread th([aw]{
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            aw->resolve(123);
        });
        coro::qt_round_robin::run();
        th.join();
        QCOMPARE(got, 123);
    }
};

QTEST_GUILESS_MAIN(TestRoundRobin)
#include "test_round_robin.moc"
