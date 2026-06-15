#include <QtTest>
#include <QBuffer>
#include <QTimer>
#include <coro/core.h>
#include <coro/iodevice.h>

class TestIoDevice : public QObject {
    Q_OBJECT
private slots:
    void awaitReadyRead() {
        QBuffer buf;
        buf.open(QIODevice::ReadWrite);
        bool woke = false;
        coro::launch([&]{
            coro::await(&buf);
            woke = true;
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ buf.write("hi"); });
        coro::exec();
        QVERIFY(woke);
        QCOMPARE(buf.size(), qint64(2));
    }
};

QTEST_GUILESS_MAIN(TestIoDevice)
#include "test_iodevice.moc"
