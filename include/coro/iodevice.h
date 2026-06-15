#pragma once
#include <memory>
#include <QIODevice>
#include <boost/fiber/all.hpp>

namespace coro {

// 等待 QIODevice 的 readyRead（单次）。返回后由调用者读取数据。
inline void await(QIODevice* dev) {
    auto pr   = std::make_shared<boost::fibers::promise<void>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = QObject::connect(dev, &QIODevice::readyRead, [pr, conn]{
        QObject::disconnect(*conn);
        pr->set_value();
    });
    fut.get();
}

} // namespace coro
