#include <QtTest>
#include <QProcess>
#include <coro/core.h>
#include <coro/process.h>

class TestProcess : public QObject {
    Q_OBJECT
private slots:
    void awaitProcessExitCode() {
        QProcess p;
        int code = -1;
        coro::launch([&]{
            p.start("sh", QStringList() << "-c" << "exit 7");
            code = coro::await(&p);   // 等 finished，返回退出码
            coro::quit();
        });
        coro::exec();
        QCOMPARE(code, 7);
    }

    void awaitProcessSuccess() {
        QProcess p;
        int code = -1;
        coro::launch([&]{
            p.start("sh", QStringList() << "-c" << "exit 0");
            code = coro::await(&p);
            coro::quit();
        });
        coro::exec();
        QCOMPARE(code, 0);
    }
};

QTEST_GUILESS_MAIN(TestProcess)
#include "test_process.moc"
