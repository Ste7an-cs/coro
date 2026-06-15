#pragma once
#include <tuple>
#include <type_traits>
#include <memory>
#include <utility>
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

// 取信号前 K 个(decay 后)参数的结果类型
template<class FullTuple, std::size_t... I>
auto prefix_result_impl(std::index_sequence<I...>)
    -> typename pack_result<std::tuple_element_t<I, FullTuple>...>::type;
template<class FullTuple, std::size_t K>
using prefix_result_t = decltype(prefix_result_impl<FullTuple>(std::make_index_sequence<K>{}));

template<class R, class Tuple, std::size_t... I>
void set_first_k(boost::fibers::promise<R>& pr, Tuple& t, std::index_sequence<I...>) {
    if constexpr (std::is_void_v<R>) { (void)t; pr.set_value(); }
    else                            { pr.set_value(R(std::get<I>(t)...)); }
}

template<std::size_t K, class Obj, class Sig, class... A>
auto await_count_impl(Obj* obj, Sig sig, std::tuple<A...>*) {
    constexpr std::size_t N = sizeof...(A);
    static_assert(K <= N, "coro::await<K>(obj, signal): K 超过信号参数个数");
    using Full = std::tuple<std::decay_t<A>...>;
    using R    = prefix_result_t<Full, K>;
    auto pr   = std::make_shared<boost::fibers::promise<R>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = QObject::connect(obj, sig, [pr, conn](A... a) {
        QObject::disconnect(*conn);
        Full all{ a... };
        set_first_k<R>(*pr, all, std::make_index_sequence<K>{});
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

// 取信号前 K 个参数(模仿 Qt 槽可少于信号参数);默认重载取全部
template<std::size_t K, class Obj, class Sig>
auto await(Obj* obj, Sig sig) {
    return detail::await_count_impl<K>(
        obj, sig, static_cast<typename detail::signal_args<Sig>::type*>(nullptr));
}

} // namespace coro
