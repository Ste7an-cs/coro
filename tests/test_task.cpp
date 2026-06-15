#include <QtTest>
#include <QTimer>
#include <stdexcept>
#include <coro/core.h>
#include <coro/signal.h>
#include <coro/task.h>

class Emitter2 : public QObject {
    Q_OBJECT
public:
signals:
    void value(int v);
};

class TestTask : public QObject {
    Q_OBJECT
private slots:
    void asyncReturnsValue() {
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([]{ return 21 * 2; });
            got = coro::await(t);
            coro::quit();
        });
        coro::exec();
        QCOMPARE(got, 42);
    }

    void awaitTaskWaitsForSignal() {
        Emitter2 e;
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([&]{ return coro::await(&e, &Emitter2::value); });
            got = coro::await(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.value(99); });
        coro::exec();
        QCOMPARE(got, 99);
    }

    void exceptionPropagatesThroughTask() {
        bool caught = false;
        coro::launch([&]{
            coro::Task<int> t = coro::async([]() -> int { throw std::runtime_error("boom"); });
            try { coro::await(t); } catch (const std::runtime_error&) { caught = true; }
            coro::quit();
        });
        coro::exec();
        QVERIFY(caught);
    }

    void thenInvokesCallbackWithResult() {
        Emitter2 e;
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([&]{ return coro::await(&e, &Emitter2::value); });
            t.then([&](int v){ got = v; coro::quit(); });
            // 注意：launch 体在此返回，then 的 watcher fiber 负责后续
        });
        QTimer::singleShot(10, [&]{ emit e.value(123); });
        coro::exec();
        QCOMPARE(got, 123);
    }
};

QTEST_GUILESS_MAIN(TestTask)
#include "test_task.moc"
