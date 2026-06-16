#pragma once
#include <tuple>
#include <type_traits>
#include <memory>
#include <utility>
#include <QObject>
#include "coro/awaitable.h"

namespace coro {
namespace detail {

// 从信号指针萃取参数类型(decay 去引用/const)
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

// 默认:取信号全部参数,经由 awaitable<R> 传递
template<class Obj, class Sig, class... A>
auto await_signal_impl(Obj* obj, Sig sig, std::tuple<A...>*) {
    using PR = pack_result<A...>;
    using R  = typename PR::type;

    auto aw    = std::make_shared<awaitable<R>>();
    auto conn  = std::make_shared<QMetaObject::Connection>();
    // 关闭守卫:连接 lambda 析构(对象销毁/断连)即关闭 awaitable,
    // 复刻旧 promise 版"对象销毁 → broken_promise"的抛出行为。
    auto guard = std::shared_ptr<void>(static_cast<void*>(nullptr),
                                       [aw](void*){ aw->close(); });

    *conn = QObject::connect(obj, sig, [aw, conn, guard](A... a) {
        QObject::disconnect(*conn);           // 单次触发
        if constexpr (std::is_void_v<R>) aw->resolve();
        else                             aw->resolve(PR::make(a...));
    });
    guard.reset();   // 仅让 lambda 持有守卫引用,避免本地变量阻止析构触发

    auto r = aw->await();
    if (r.closed()) throw awaitable_closed("coro::await(signal): awaitable closed");
    if constexpr (std::is_void_v<R>) { return; }
    else { return std::move(r).value(); }
}

// 用接收到的信号前 K 个参数构造结果(各自转换为对应 Want 形参类型)
template<class R, class... Want, class Tuple, std::size_t... I>
R make_typed(const Tuple& t, std::index_sequence<I...>) {
    return R(static_cast<std::decay_t<Want>>(std::get<I>(t))...);
}

// 指定形参类型 Want...(像 Qt 槽的形参列表):返回 pack_result<Want...>。
template<class Obj, class Sig, class... Want, class... A>
auto await_typed_impl(Obj* obj, Sig sig, std::tuple<Want...>*, std::tuple<A...>*) {
    constexpr std::size_t K = sizeof...(Want);
    constexpr std::size_t N = sizeof...(A);
    static_assert(K <= N, "coro::await<Types...>(obj, signal): 指定的形参个数超过信号参数个数");
    using R = typename pack_result<std::decay_t<Want>...>::type;   // K>=1 → 值 / tuple

    auto aw    = std::make_shared<awaitable<R>>();
    auto conn  = std::make_shared<QMetaObject::Connection>();
    auto guard = std::shared_ptr<void>(static_cast<void*>(nullptr),
                                       [aw](void*){ aw->close(); });

    *conn = QObject::connect(obj, sig, [aw, conn, guard](A... a) {
        QObject::disconnect(*conn);
        std::tuple<std::decay_t<A>...> all{ a... };
        aw->resolve(make_typed<R, Want...>(all, std::make_index_sequence<K>{}));
    });
    guard.reset();   // 仅让 lambda 持有守卫引用

    auto r = aw->await();
    if (r.closed()) throw awaitable_closed("coro::await<Types...>(signal): awaitable closed");
    return std::move(r).value();
}

} // namespace detail

// 默认:返回信号全部参数（0→void，1→值，N→std::tuple）
template<class Obj, class Sig>
auto await(Obj* obj, Sig sig) {
    return detail::await_signal_impl(
        obj, sig, static_cast<typename detail::signal_args<Sig>::type*>(nullptr));
}

// 像 Qt 槽一样指定形参类型:返回这些类型(1→值,N→tuple)。
// 例:await<int>(obj, &T::twoArg) 只取第一个参数;await<int,QString>(...) 取前两个。
// 形参类型即返回类型,且可与信号参数做转换(如 int→double)。
template<class W0, class... Wr, class Obj, class Sig>
auto await(Obj* obj, Sig sig) {
    return detail::await_typed_impl(
        obj, sig,
        static_cast<std::tuple<W0, Wr...>*>(nullptr),
        static_cast<typename detail::signal_args<Sig>::type*>(nullptr));
}

} // namespace coro
