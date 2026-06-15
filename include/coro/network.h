#pragma once
#include <memory>
#include <QNetworkReply>
#include <boost/fiber/all.hpp>

namespace coro {

// 等待 QNetworkReply 完成（finished）。单次语义。
// finished 即使在出错时也会触发；返回后由调用者 readAll()/检查 error()。
// 需要链接 Qt5::Network；本头文件为可选(opt-in)，不在 coro/coro.h 伞头文件中。
inline void await(QNetworkReply* reply) {
    if (reply->isFinished()) return;
    auto pr   = std::make_shared<boost::fibers::promise<void>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = QObject::connect(reply, &QNetworkReply::finished, [pr, conn]{
        QObject::disconnect(*conn);
        pr->set_value();
    });
    fut.get();
}

} // namespace coro
