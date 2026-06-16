#include <QtTest>
#include <coro/awaitable.h>

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

        auto vv = coro::result<void>::value();
        QVERIFY(vv.has_value());
        auto vc = coro::result<void>::closed_();
        QVERIFY(vc.closed());
    }
};

QTEST_GUILESS_MAIN(TestAwaitable)
#include "test_awaitable.moc"
