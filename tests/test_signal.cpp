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
    void threeArg(int a, const QString& b, double c);
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

    void awaitFirstOfTwo() {
        Emitter e;
        int got = -1;
        coro::launch([&]{ got = coro::await<int>(&e, &Emitter::twoArg); coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.twoArg(5, QStringLiteral("hi")); });
        coro::exec();
        QCOMPARE(got, 5);
    }

    // 指定前两个形参类型:返回 std::tuple<int, QString>
    void awaitFirstTwoOfThreeTyped() {
        Emitter e;
        int a = -1; QString b;
        coro::launch([&]{
            auto t = coro::await<int, QString>(&e, &Emitter::threeArg);
            a = std::get<0>(t); b = std::get<1>(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.threeArg(1, QStringLiteral("x"), 3.14); });
        coro::exec();
        QCOMPARE(a, 1);
        QCOMPARE(b, QStringLiteral("x"));
    }

    // 指定全部三个形参类型
    void awaitAllOfThreeTyped() {
        Emitter e;
        int a = -1; QString b; double c = 0;
        coro::launch([&]{
            auto t = coro::await<int, QString, double>(&e, &Emitter::threeArg);
            a = std::get<0>(t); b = std::get<1>(t); c = std::get<2>(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.threeArg(7, QStringLiteral("y"), 2.5); });
        coro::exec();
        QCOMPARE(a, 7);
        QCOMPARE(b, QStringLiteral("y"));
        QCOMPARE(c, 2.5);
    }

    // 形参类型可与信号参数转换:信号发 int(7),形参指定 double → 返回 7.0
    void awaitConvertedTyped() {
        Emitter e;
        double got = 0;
        coro::launch([&]{ got = coro::await<double>(&e, &Emitter::oneArg); coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.oneArg(7); });
        coro::exec();
        QCOMPARE(got, 7.0);
    }

    // 等待期间对象被销毁:连接 lambda 析构 → awaitable 关闭 → await 抛 awaitable_closed
    void closedThrowsOnDestroy() {
        bool threw = false;
        auto* e = new Emitter;
        coro::launch([&]{
            try { coro::await(e, &Emitter::oneArg); }
            catch (const coro::awaitable_closed&) { threw = true; }
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ delete e; });
        coro::exec();
        QVERIFY(threw);
    }

    // 同上,但走 await<Types...> 的 typed 路径(await_typed_impl 的关闭守卫)
    void closedThrowsOnDestroyTyped() {
        bool threw = false;
        auto* e = new Emitter;
        coro::launch([&]{
            try { coro::await<int>(e, &Emitter::oneArg); }
            catch (const coro::awaitable_closed&) { threw = true; }
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ delete e; });
        coro::exec();
        QVERIFY(threw);
    }
};

QTEST_GUILESS_MAIN(TestSignal)
#include "test_signal.moc"
