// coro::sync_awaitable<T>:基于 coro::sync_channel(std::mutex+cv,阻塞 OS 线程)的跨线程 awaitable。
// 与 coro::awaitable 不同,它的 await()/resolve() 可在【非 fiber 线程】调用,不依赖 fiber 调度器。
// 本测试故意把消费者放在普通 std::thread 上 await —— 这是 boost.fiber unbuffered_channel 做不到的。
#include <QtTest>
#include <coro/awaitable.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

class TestSyncAwaitable : public QObject {
    Q_OBJECT
private slots:
    // 消费者在【非 fiber】std::thread 上 await,生产者在主线程 resolve(rendezvous)。
    void awaitFromNonFiberThread() {
        auto aw = std::make_shared<coro::sync_awaitable<int>>();
        int got = -1;
        bool hasVal = false;
        std::thread consumer([&] {
            auto r = aw->await();       // 非 fiber 线程阻塞取值
            hasVal = r.has_value();
            if (hasVal) got = r.value();
        });
        std::this_thread::sleep_for(20ms);   // 让 consumer 先 park 在 await
        bool ok = aw->resolve(42);           // rendezvous:阻塞至被取走
        consumer.join();
        QVERIFY(ok);
        QVERIFY(hasVal);
        QCOMPARE(got, 42);
    }

    // 可复用:连续多次 rendezvous。
    void reuseMultiShot() {
        auto aw = std::make_shared<coro::sync_awaitable<int>>();
        std::vector<int> got;
        std::thread consumer([&] {
            for (auto r = aw->await(); r.has_value(); r = aw->await())
                got.push_back(r.value());
        });
        for (int i = 0; i < 5; ++i)
            QVERIFY(aw->resolve(i * 10));
        aw->close();
        consumer.join();
        QCOMPARE(got.size(), std::size_t(5));
        for (int i = 0; i < 5; ++i) QCOMPARE(got[i], i * 10);
    }

    // close() 唤醒阻塞中的 await → closed。
    void closeWakesAwaiter() {
        auto aw = std::make_shared<coro::sync_awaitable<int>>();
        bool gotClosed = false;
        std::thread consumer([&] {
            auto r = aw->await();
            gotClosed = r.closed();
        });
        std::this_thread::sleep_for(20ms);
        aw->close();
        consumer.join();
        QVERIFY(gotClosed);
    }

    // 关闭后 resolve 返回 false。
    void resolveAfterCloseFails() {
        auto aw = std::make_shared<coro::sync_awaitable<int>>();
        aw->close();
        QVERIFY(!aw->resolve(1));
    }

    // void 特化。
    void voidSpecialization() {
        auto aw = std::make_shared<coro::sync_awaitable<void>>();
        bool woke = false;
        std::thread consumer([&] {
            auto r = aw->await();
            woke = r.has_value();
        });
        std::this_thread::sleep_for(20ms);
        QVERIFY(aw->resolve());
        consumer.join();
        QVERIFY(woke);
    }
};

QTEST_GUILESS_MAIN(TestSyncAwaitable)
#include "test_sync_awaitable.moc"
