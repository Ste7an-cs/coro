#include <QtTest>
#include <QtConcurrent>
#include <QFuture>
#include <stdexcept>
#include <coro/core.h>
#include <coro/future.h>

class TestFuture : public QObject {
    Q_OBJECT
private slots:
    void awaitFutureResult() {
        int got = 0;
        coro::launch([&]{
            QFuture<int> f = QtConcurrent::run([]{ return 21 * 2; });
            got = coro::await(f);
            coro::quit();
        });
        coro::exec();
        QCOMPARE(got, 42);
    }

    void awaitVoidFuture() {
        bool ran = false;
        coro::launch([&]{
            QFuture<void> f = QtConcurrent::run([]{ /* 副作用 */ });
            coro::await(f);
            ran = true;
            coro::quit();
        });
        coro::exec();
        QVERIFY(ran);
    }
};

QTEST_GUILESS_MAIN(TestFuture)
#include "test_future.moc"
