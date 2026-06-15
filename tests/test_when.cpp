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

    void whenAnyReturnsFirstIndex() {
        size_t idx = SIZE_MAX;
        int winnerVal = 0;
        coro::launch([&]{
            coro::Task<int> slow = coro::async([]{ coro::sleep(std::chrono::milliseconds(80)); return 10; });
            coro::Task<int> fast = coro::async([]{ coro::sleep(std::chrono::milliseconds(20)); return 20; });
            idx = coro::whenAny(slow, fast);
            // 胜者结果仍可取（whenAny 不消费 future）
            winnerVal = (idx == 1) ? fast.get() : slow.get();
            // 等败者 fiber 完成再退出：否则未完成的 detached async fiber 会在
            // boost.fibers 静态调度器析构时空转（事件循环已停，其 80ms 定时器永不触发）。
            slow.wait();
            coro::quit();
        });
        coro::exec();
        QCOMPARE(idx, size_t(1));      // fast 先完成
        QCOMPARE(winnerVal, 20);
    }
};

QTEST_GUILESS_MAIN(TestWhen)
#include "test_when.moc"
