#pragma once
#include <array>
#include <memory>
#include <QByteArray>
#include <QIODevice>
#include <QPointer>
#include <boost/fiber/all.hpp>
#include "coro/awaitable.h"

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

// 字节块生成器：每拉取一次挂起直到下一次 readyRead，产出 dev->readAll()。
// 流结束（readChannelFinished / 设备销毁）→ next() 返回 closed()，绝不抛异常。
class io_byte_generator {
public:
    explicit io_byte_generator(QIODevice* dev) : dev_(dev) {}
    io_byte_generator(io_byte_generator&&) noexcept = default;
    io_byte_generator& operator=(io_byte_generator&&) noexcept = default;
    io_byte_generator(const io_byte_generator&) = delete;
    io_byte_generator& operator=(const io_byte_generator&) = delete;

    // 显式拉取：挂起直到下一块；流结束 → result.closed()。
    result<QByteArray> next() {
        for (;;) {
            if (!dev_) return result<QByteArray>::closed_();           // 已销毁
            if (dev_->bytesAvailable() > 0)
                return result<QByteArray>::value(dev_->readAll());    // 先排空缓冲
            if (eos_) return result<QByteArray>::closed_();           // 结束且无残留

            // 在单次 awaitable 上汇合 readyRead / readChannelFinished / destroyed。
            // 槽内先断开全部连接，避免第二个槽对已被消费的 rendezvous 通道再次 push 而挂起。
            awaitable<int> aw;   // 1 = 可读，0 = 流结束
            auto conns = std::make_shared<std::array<QMetaObject::Connection, 3>>();
            auto disconnectAll = [conns]{ for (auto& c : *conns) QObject::disconnect(c); };

            (*conns)[0] = QObject::connect(dev_, &QIODevice::readyRead,
                [&aw, disconnectAll]{ disconnectAll(); aw.resolve(1); });
            (*conns)[1] = QObject::connect(dev_, &QIODevice::readChannelFinished,
                [&aw, disconnectAll]{ disconnectAll(); aw.resolve(0); });
            (*conns)[2] = QObject::connect(dev_, &QObject::destroyed,
                [&aw, disconnectAll]{ disconnectAll(); aw.close(); });

            auto r = aw.await();
            disconnectAll();   // 幂等：正常唤醒时槽已断开；销毁时连接已失效

            if (r.closed()) return result<QByteArray>::closed_();     // destroyed
            if (r.value() == 0) eos_ = true;                          // 结束：回环排空残留
            // 否则回环 → readAll()
        }
    }

private:
    QPointer<QIODevice> dev_;
    bool eos_ = false;
};

inline io_byte_generator generate(QIODevice* dev) {
    return io_byte_generator(dev);
}

} // namespace coro
