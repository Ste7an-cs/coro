#include <QtTest>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <coro/core.h>
#include <coro/iodevice.h>

// coro::generate(QIODevice*) 针对真实 QTcpSocket 的回环验证(opt-in，需链接 Qt5::Network)。
// 服务端接受连接后写入数据并断开;客户端协程用生成器收集字节,
// 断开触发 readChannelFinished → 生成器正常结束。
class TestIoDeviceSocket : public QObject {
    Q_OBJECT
private slots:
    void generateOverTcpSocket() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port = server.serverPort();

        // 服务端:新连接到来即写数据并断开(socket 以 server 为父对象,自动回收)。
        QObject::connect(&server, &QTcpServer::newConnection, [&]{
            QTcpSocket* s = server.nextPendingConnection();
            s->write("hello socket");
            s->disconnectFromHost();
        });

        QTcpSocket client;
        QByteArray received;
        bool ended = false, threw = false, timedOut = false;

        // 看门狗:避免连接异常时无限挂起。
        QTimer::singleShot(5000, [&]{ timedOut = true; coro::quit(); });

        coro::launch([&]{
            client.connectToHost(QHostAddress::LocalHost, port);
            try {
                for (QByteArray c : coro::generate(&client)) received += c;
                ended = true;
            } catch (...) { threw = true; }
            coro::quit();
        });

        coro::exec();

        QVERIFY(!timedOut);
        QVERIFY(!threw);
        QVERIFY(ended);
        QCOMPARE(received, QByteArray("hello socket"));
    }
};

QTEST_GUILESS_MAIN(TestIoDeviceSocket)
#include "test_iodevice_socket.moc"
