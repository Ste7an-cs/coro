#pragma once
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>          // std::monostate
#include <boost/fiber/all.hpp>

namespace coro {

// await() 在 awaitable 关闭后被解包消费时抛出(取代旧 promise 版的 broken_promise)
class awaitable_closed : public std::runtime_error {
public:
    explicit awaitable_closed(const char* msg) : std::runtime_error(msg) {}
};

// 值-或-关闭的结果类型(不携带异常)
template<class T>
class result {
public:
    static result value(T v) { result r; r.v_ = std::move(v); return r; }
    static result closed_()  { return result{}; }

    bool has_value() const noexcept { return v_.has_value(); }
    bool closed()    const noexcept { return !v_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!v_) throw awaitable_closed("coro::result: value() on closed result");
        return *v_;
    }
    T&& value() && {
        if (!v_) throw awaitable_closed("coro::result: value() on closed result");
        return std::move(*v_);
    }
    const T& value() const& {
        if (!v_) throw awaitable_closed("coro::result: value() on closed result");
        return *v_;
    }
private:
    std::optional<T> v_;
};

// void 特化:仅携带 closed/has_value 状态
template<>
class result<void> {
public:
    static result value()   { result r; r.has_ = true;  return r; }
    static result closed_()  { result r; r.has_ = false; return r; }
    bool has_value() const noexcept { return has_; }
    bool closed()    const noexcept { return !has_; }
    explicit operator bool() const noexcept { return has_; }
private:
    bool has_ = false;
};

// 可复用的异步数据同步原语:基于 unbuffered_channel(rendezvous)。
// 要求 T 可默认构造(信号实参经 std::decay 后均满足)。
template<class T>
class awaitable {
public:
    static_assert(std::is_default_constructible_v<T>,
                  "awaitable<T>: T 必须可默认构造(await() 内 'T v;' 的要求,与 unbuffered_channel 无关)");
    awaitable() = default;
    awaitable(const awaitable&) = delete;
    awaitable& operator=(const awaitable&) = delete;

    // 协程侧:取一个值;通道关闭且无值 → closed()。
    result<T> await() {
        T v;
        auto st = ch_.pop(v);
        if (st == boost::fibers::channel_op_status::success)
            return result<T>::value(std::move(v));
        return result<T>::closed_();
    }

    // 生产侧:提交一个值(rendezvous,push 直到被消费);通道已关闭 → false。
    bool resolve(T v) {
        return ch_.push(std::move(v)) == boost::fibers::channel_op_status::success;
    }

    // 关闭通道:正在/后续 await 得到 closed();后续 resolve 返回 false。幂等。
    void close() { ch_.close(); }

private:
    boost::fibers::unbuffered_channel<T> ch_;
};

// void 特化:内部用 unbuffered_channel<std::monostate>
template<>
class awaitable<void> {
public:
    awaitable() = default;
    awaitable(const awaitable&) = delete;
    awaitable& operator=(const awaitable&) = delete;

    result<void> await() {
        std::monostate m;
        auto st = ch_.pop(m);
        if (st == boost::fibers::channel_op_status::success)
            return result<void>::value();
        return result<void>::closed_();
    }

    bool resolve() {
        return ch_.push(std::monostate{}) == boost::fibers::channel_op_status::success;
    }

    void close() { ch_.close(); }

private:
    boost::fibers::unbuffered_channel<std::monostate> ch_;
};

} // namespace coro
