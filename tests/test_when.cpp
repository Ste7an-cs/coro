#include <QtTest>
#include <QElapsedTimer>
#include <coro/core.h>
#include <coro/task.h>
#include <coro/when.h>

class TestWhen : public QObject {
    Q_OBJECT
private slots:
    void whenAllReturnsAllResults() {
        int a = 0, b = 0;
        QElapsedTimer timer; timer.start();
        coro::launch([&]{
            coro::Task<int> ta = coro::async([]{ coro::sleep(std::chrono::milliseconds(50)); return 1; });
            coro::Task<int> tb = coro::async([]{ coro::sleep(std::chrono::milliseconds(50)); return 2; });
            auto [ra, rb] = coro::whenAll(ta, tb);
            a = ra; b = rb;
            coro::quit();
        });
        coro::exec();
        QCOMPARE(a, 1);
        QCOMPARE(b, 2);
        QVERIFY(timer.elapsed() < 150);   // 并发：约 50ms，而非串行 100ms
    }
};

QTEST_GUILESS_MAIN(TestWhen)
#include "test_when.moc"
