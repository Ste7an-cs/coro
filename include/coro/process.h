#pragma once
#include <memory>
#include <QProcess>
#include <boost/fiber/all.hpp>

namespace coro {

// 等待 QProcess 结束（finished），返回退出码。单次语义。
// 注意：QProcess 是 QIODevice，但本重载精确匹配 QProcess* 用于"等结束"；
// 若要等其输出可读，使用 await(static_cast<QIODevice*>(proc))。
inline int await(QProcess* proc) {
    auto pr   = std::make_shared<boost::fibers::promise<int>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = QObject::connect(
        proc,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [pr, conn](int exitCode, QProcess::ExitStatus) {
            QObject::disconnect(*conn);
            pr->set_value(exitCode);
        });
    return fut.get();
}

} // namespace coro
