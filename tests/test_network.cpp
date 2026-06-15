#include <QtTest>
#include <QTemporaryFile>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <coro/core.h>
#include <coro/network.h>

class TestNetwork : public QObject {
    Q_OBJECT
private slots:
    void awaitFileReply() {
        QTemporaryFile tf;
        QVERIFY(tf.open());
        tf.write("hello");
        tf.flush();
        const QString path = tf.fileName();

        QNetworkAccessManager mgr;
        QByteArray body;
        coro::launch([&]{
            QNetworkReply* r = mgr.get(QNetworkRequest(QUrl::fromLocalFile(path)));
            coro::await(r);                 // 等 finished
            body = r->readAll();
            r->deleteLater();
            coro::quit();
        });
        coro::exec();
        QCOMPARE(body, QByteArray("hello"));
    }
};

QTEST_GUILESS_MAIN(TestNetwork)
#include "test_network.moc"
