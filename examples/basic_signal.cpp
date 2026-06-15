#include <QCoreApplication>
#include <QTimer>
#include <coro/coro.h>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    coro::launch([]{
        std::puts("协程启动，等 500ms ...");
        coro::sleep(std::chrono::milliseconds(500));
        std::puts("醒来，结束。");
        coro::quit();
    });

    return coro::exec();   // 取代 app.exec()
}
