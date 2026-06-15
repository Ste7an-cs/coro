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

// 默认:取信号全部参数
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

// 用接收到的信号前 sizeof...(Want) 个参数构造结果(各自转换为对应的 Want 形参类型)
template<class R, class... Want, class Tuple, std::size_t... I>
void set_typed(boost::fibers::promise<R>& pr, Tuple& t, std::index_sequence<I...>) {
    pr.set_value(R(static_cast<std::decay_t<Want>>(std::get<I>(t))...));
}

// 指定形参类型 Want...(像 Qt 槽的形参列表):返回 pack_result<Want...>。
// Want... 须是信号前若干个参数且可隐式/static_cast 转换,模仿"槽形参可少于信号参数"。
template<class Obj, class Sig, class... Want, class... A>
auto await_typed_impl(Obj* obj, Sig sig, std::tuple<Want...>*, std::tuple<A...>*) {
    constexpr std::size_t K = sizeof...(Want);
    constexpr std::size_t N = sizeof...(A);
    static_assert(K <= N, "coro::await<Types...>(obj, signal): 指定的形参个数超过信号参数个数");
    using R = typename pack_result<std::decay_t<Want>...>::type;   // K>=1 → 值 / tuple

    auto pr   = std::make_shared<boost::fibers::promise<R>>();
    auto fut  = pr->get_future();
    auto conn = std::make_shared<QMetaObject::Connection>();

    *conn = QObject::connect(obj, sig, [pr, conn](A... a) {
        QObject::disconnect(*conn);
        std::tuple<std::decay_t<A>...> all{ a... };
        set_typed<R, Want...>(*pr, all, std::make_index_sequence<K>{});
    });
    return fut.get();
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
