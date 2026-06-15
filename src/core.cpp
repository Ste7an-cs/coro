#include "coro/core.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QAbstractEventDispatcher>
#include <boost/fiber/all.hpp>
#include <memory>

namespace {
struct DriverState { bool running = false; int code = 0; };
DriverState& state() { static DriverState s; return s; }
}

namespace coro {

int exec() {
    auto& s = state();
    s.running = true;
    s.code = 0;
    while (s.running) {
        boost::this_fiber::yield();
        if (!s.running) break;
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents | QEventLoop::AllEvents);
    }
    return s.code;
}

void quit(int code) {
    auto& s = state();
    s.code = code;
    s.running = false;
    if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
}

void launch(std::function<void()> fn) {
    boost::fibers::fiber(std::move(fn)).detach();
}

void yield() {
    if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
    boost::this_fiber::yield();
}

void sleep(std::chrono::milliseconds ms) {
    boost::fibers::promise<void> p;
    auto f = p.get_future();
    auto pp = std::make_shared<boost::fibers::promise<void>>(std::move(p));
    QTimer::singleShot(static_cast<int>(ms.count()), [pp]{ pp->set_value(); });
    f.get();
}

}
