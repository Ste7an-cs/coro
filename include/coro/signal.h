#pragma once
#include <tuple>
#include <type_traits>
#include <memory>
#include <QObject>
#include <boost/fiber/all.hpp>

namespace coro {
namespace detail {

// 从信号指针(指向成员函数)萃取参数类型(decay 去引用/const)
template<class T> struct signal_args;
template<class C, class R, class... A>
struct signal_args<R (C::*)(A...)> { using type = std::tuple<std::decay_t<A>...>; };

// 按参数个数决定 await 返回类型与打包方式
template<class... A> struct pack_result {            // N 个 → tuple
    using type = std::tuple<A...>;
    static type make(A... a) { return type(a...); }
};
template<class A> struct pack_result<A> {            // 1 个 → 该值
    using type = A;
    static type make(A a) { return a; }
};
template<> struct pack_result<> {                    // 0 个 → void
    using type = void;
};

template<class Obj, class Sig, class... A>
auto await_signal_impl(Obj* obj, Sig sig, std::tuple<A...>*) {
    using PR = pack_result<A...>;
    using R  = typename PR::type;

    auto pr   = std::make_shared<boost::fibers::promise<R>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();

    *conn = QObject::connect(obj, sig, [pr, conn](A... a) {
        QObject::disconnect(*conn);           // 单次触发
        if constexpr (std::is_void_v<R>) pr->set_value();
        else                             pr->set_value(PR::make(a...));
    });

    if constexpr (std::is_void_v<R>) { fut.get(); }
    else                            { return fut.get(); }
}

} // namespace detail

// 通用信号 await：返回信号参数（0→void，1→值，N→std::tuple）
template<class Obj, class Sig>
auto await(Obj* obj, Sig sig) {
    return detail::await_signal_impl(
        obj, sig, static_cast<typename detail::signal_args<Sig>::type*>(nullptr));
}

} // namespace coro
