#include <QtTest>
#include <boost/fiber/all.hpp>

class TestDriver : public QObject {
    Q_OBJECT
private slots:
    void boostFiberLinks() {
        auto f = boost::fibers::async([]{ return 42; });
        QCOMPARE(f.get(), 42);
    }
};

QTEST_GUILESS_MAIN(TestDriver)
#include "test_driver.moc"
