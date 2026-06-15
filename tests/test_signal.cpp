#include <QtTest>
#include <QTimer>
#include <coro/core.h>
#include <coro/signal.h>

class Emitter : public QObject {
    Q_OBJECT
public:
signals:
    void noArg();
    void oneArg(int v);
    void twoArg(int a, const QString& b);
};

class TestSignal : public QObject {
    Q_OBJECT
private slots:
    void awaitOneArg() {
        Emitter e;
        int got = -1;
        coro::launch([&]{ got = coro::await(&e, &Emitter::oneArg); coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.oneArg(7); });
        coro::exec();
        QCOMPARE(got, 7);
    }

    void awaitNoArg() {
        Emitter e;
        bool woke = false;
        coro::launch([&]{ coro::await(&e, &Emitter::noArg); woke = true; coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.noArg(); });
        coro::exec();
        QVERIFY(woke);
    }

    void awaitTwoArgs() {
        Emitter e;
        int a = -1; QString b;
        coro::launch([&]{
            auto t = coro::await(&e, &Emitter::twoArg);
            a = std::get<0>(t); b = std::get<1>(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.twoArg(5, QStringLiteral("hi")); });
        coro::exec();
        QCOMPARE(a, 5);
        QCOMPARE(b, QStringLiteral("hi"));
    }
};

QTEST_GUILESS_MAIN(TestSignal)
#include "test_signal.moc"
