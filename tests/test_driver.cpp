#include <QtTest>
#include <coro/core.h>
#include <boost/fiber/all.hpp>

class TestDriver : public QObject {
    Q_OBJECT
private slots:
    void boostFiberLinks() {
        auto f = boost::fibers::async([]{ return 42; });
        QCOMPARE(f.get(), 42);
    }
    void launchRunsAndQuits() {
        bool ran = false;
        int order = 0;
        coro::launch([&]{
            ran = true;
            order = 1;
            coro::quit(7);
        });
        int rc = coro::exec();
        QVERIFY(ran);
        QCOMPARE(order, 1);
        QCOMPARE(rc, 7);
    }
};

QTEST_GUILESS_MAIN(TestDriver)
#include "test_driver.moc"
