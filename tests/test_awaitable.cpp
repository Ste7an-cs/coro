#include <QtTest>
#include <QTimer>
#include <coro/awaitable.h>
#include <coro/core.h>
#include <memory>
#include <vector>

class TestAwaitable : public QObject {
    Q_OBJECT
private slots:
    void resultValueAndClosed() {
        auto v = coro::result<int>::value(5);
        QVERIFY(v.has_value());
        QVERIFY(!v.closed());
        QVERIFY(static_cast<bool>(v));
        QCOMPARE(v.value(), 5);

        auto c = coro::result<int>::closed_();
        QVERIFY(!c.has_value());
        QVERIFY(c.closed());
        QVERIFY(!static_cast<bool>(c));
        QVERIFY_EXCEPTION_THROWN(c.value(), coro::awaitable_closed);  // closed → value() 抛

        auto vv = coro::result<void>::value();
        QVERIFY(vv.has_value());
        auto vc = coro::result<void>::closed_();
        QVERIFY(vc.closed());
    }

    void basicResolveAwait() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        int got = -1;
        coro::launch([&, aw]{
            auto r = aw->await();
            QVERIFY(r.has_value());
            got = r.value();
            coro::quit();
        });
        QTimer::singleShot(10, [aw]{ aw->resolve(42); });
        coro::exec();
        QCOMPARE(got, 42);
    }

    void reuseMultiShot() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        std::vector<int> got;
        coro::launch([&, aw]{
            for (int i = 0; i < 3; ++i) {
                auto r = aw->await();
                got.push_back(r.value());
            }
            coro::quit();
        });
        QTimer::singleShot(10, [aw]{ aw->resolve(0); });
        QTimer::singleShot(20, [aw]{ aw->resolve(10); });
        QTimer::singleShot(30, [aw]{ aw->resolve(20); });
        coro::exec();
        QCOMPARE(got.size(), std::size_t(3));
        QCOMPARE(got[0], 0);
        QCOMPARE(got[1], 10);
        QCOMPARE(got[2], 20);
    }

    void voidSpecialization() {
        auto aw = std::make_shared<coro::awaitable<void>>();
        bool woke = false;
        coro::launch([&, aw]{
            auto r = aw->await();
            woke = r.has_value();
            coro::quit();
        });
        QTimer::singleShot(10, [aw]{ aw->resolve(); });
        coro::exec();
        QVERIFY(woke);
    }

    void closeWakesAwaiter() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        bool gotClosed = false;
        coro::launch([&, aw]{
            auto r = aw->await();
            gotClosed = r.closed();
            coro::quit();
        });
        QTimer::singleShot(10, [aw]{ aw->close(); });
        coro::exec();
        QVERIFY(gotClosed);
    }

    void resolveAfterCloseFails() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        bool resolveOk = true;
        coro::launch([&, aw]{
            aw->close();
            resolveOk = aw->resolve(1);   // 关闭后提交 → false
            coro::quit();
        });
        coro::exec();
        QVERIFY(!resolveOk);
    }

    // 先 close 再 await:pop 在已关闭的空通道上应立即返回 closed(不阻塞)
    void awaitAfterClose() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        bool gotClosed = false;
        coro::launch([&, aw]{
            aw->close();
            auto r = aw->await();
            gotClosed = r.closed();
            coro::quit();
        });
        coro::exec();
        QVERIFY(gotClosed);
    }

    // awaitable<void> 的 close 也应唤醒等待者并得到 closed()
    void voidCloseWakesAwaiter() {
        auto aw = std::make_shared<coro::awaitable<void>>();
        bool gotClosed = false;
        coro::launch([&, aw]{
            auto r = aw->await();
            gotClosed = r.closed();
            coro::quit();
        });
        QTimer::singleShot(10, [aw]{ aw->close(); });
        coro::exec();
        QVERIFY(gotClosed);
    }
};

QTEST_GUILESS_MAIN(TestAwaitable)
#include "test_awaitable.moc"
