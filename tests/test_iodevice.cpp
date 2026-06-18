#include <QtTest>
#include <QBuffer>
#include <QTimer>
#include <QByteArray>
#include <cstring>
#include <coro/core.h>
#include <coro/iodevice.h>

// 顺序 QIODevice 桩：feed() 投递数据并发 readyRead；finish() 发 readChannelFinished。
class SeqDevice : public QIODevice {
    Q_OBJECT
public:
    explicit SeqDevice(QObject* p = nullptr) : QIODevice(p) { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return buf_.size() + QIODevice::bytesAvailable(); }
    void feed(const QByteArray& d) { buf_.append(d); emit readyRead(); }
    void finish() { emit readChannelFinished(); }
protected:
    qint64 readData(char* data, qint64 max) override {
        qint64 n = qMin<qint64>(max, qint64(buf_.size()));
        std::memcpy(data, buf_.constData(), size_t(n));
        buf_.remove(0, int(n));
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray buf_;
};

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

    void nextYieldsChunks() {
        SeqDevice dev;
        QList<QByteArray> got;
        coro::launch([&]{
            auto gen = coro::generate(&dev);
            while (auto r = gen.next()) got.append(std::move(r).value());
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev.feed("aa"); });
        QTimer::singleShot(20, [&]{ dev.feed("bbb"); });
        QTimer::singleShot(30, [&]{ dev.finish(); });
        coro::exec();
        QCOMPARE(got.size(), 2);
        QCOMPARE(got[0], QByteArray("aa"));
        QCOMPARE(got[1], QByteArray("bbb"));
    }

    void destroyEndsClean() {
        auto* dev = new SeqDevice;
        bool ended = false, threw = false;
        coro::launch([&]{
            auto gen = coro::generate(dev);
            try { while (auto r = gen.next()) { (void)r; } ended = true; }
            catch (...) { threw = true; }
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev->feed("x"); });
        QTimer::singleShot(20, [&]{ delete dev; });
        coro::exec();
        QVERIFY(ended);
        QVERIFY(!threw);
    }

    void rangeForCollects() {
        SeqDevice dev;
        QByteArray all;
        coro::launch([&]{
            for (QByteArray c : coro::generate(&dev)) all += c;
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev.feed("hello "); });
        QTimer::singleShot(20, [&]{ dev.feed("world"); });
        QTimer::singleShot(30, [&]{ dev.finish(); });
        coro::exec();
        QCOMPARE(all, QByteArray("hello world"));
    }
};

QTEST_GUILESS_MAIN(TestIoDevice)
#include "test_iodevice.moc"
