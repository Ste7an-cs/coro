#include <QtTest>
#include <QElapsedTimer>
#include <coro/core.h>
#include <vector>

class TestSleep : public QObject {
    Q_OBJECT
private slots:
    void concurrentSleepInterleaves() {
        std::vector<int> finished;
        int remaining = 2;
        auto done = [&]{ if (--remaining == 0) coro::quit(); };

        // 协程 A：睡 60ms 后记录 1
        coro::launch([&]{ coro::sleep(std::chrono::milliseconds(60)); finished.push_back(1); done(); });
        // 协程 B：睡 20ms 后记录 2（应先于 A 完成 → 证明 A 的 sleep 没阻塞 B）
        coro::launch([&]{ coro::sleep(std::chrono::milliseconds(20)); finished.push_back(2); done(); });

        QElapsedTimer t; t.start();
        coro::exec();

        QCOMPARE(finished.size(), size_t(2));
        QCOMPARE(finished[0], 2);   // B 先完成
        QCOMPARE(finished[1], 1);   // A 后完成
        // 并发：总耗时约 60ms 量级，远小于串行的 80ms
        QVERIFY(t.elapsed() < 200);
    }
};

QTEST_GUILESS_MAIN(TestSleep)
#include "test_sleep.moc"
