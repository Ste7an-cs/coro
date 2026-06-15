#include <QCoreApplication>
#include <QtConcurrent>
#include <coro/coro.h>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    coro::launch([]{
        coro::Task<int> a = coro::async([]{ coro::sleep(std::chrono::milliseconds(100)); return 1; });
        coro::Task<int> b = coro::async([]{ coro::sleep(std::chrono::milliseconds(100)); return 2; });
        auto [ra, rb] = coro::whenAll(a, b);
        std::printf("whenAll -> %d, %d\n", ra, rb);

        QFuture<int> f = QtConcurrent::run([]{ return 42; });
        std::printf("QFuture -> %d\n", coro::await(f));

        coro::quit();
    });

    return coro::exec();
}
