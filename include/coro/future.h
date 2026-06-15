#pragma once
#include <QFuture>
#include <QFutureWatcher>
#include <memory>
#include <boost/fiber/all.hpp>

namespace coro {

// await 一个返回值的 QFuture<T>
template<class T>
T await(const QFuture<T>& f) {
    QFuture<T> fut = f;
    if (fut.isFinished()) return fut.result();

    QFutureWatcher<T> watcher;
    auto pr  = std::make_shared<boost::fibers::promise<void>>();
    auto pfu = pr->get_future();
    QObject::connect(&watcher, &QFutureWatcherBase::finished, [pr]{ pr->set_value(); });
    watcher.setFuture(fut);
    pfu.get();
    return fut.result();
}

// await 一个 QFuture<void>（非模板，优先于上面的模板被选中）
inline void await(const QFuture<void>& f) {
    QFuture<void> fut = f;
    if (fut.isFinished()) return;

    QFutureWatcher<void> watcher;
    auto pr  = std::make_shared<boost::fibers::promise<void>>();
    auto pfu = pr->get_future();
    QObject::connect(&watcher, &QFutureWatcherBase::finished, [pr]{ pr->set_value(); });
    watcher.setFuture(fut);
    pfu.get();
}

} // namespace coro
