# awaitable<T> Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 引入可复用的异步数据同步原语 `coro::awaitable<T>`(基于 `boost::fibers::unbuffered_channel`),并把信号适配器 `coro::await(obj, signal)` 的底层从 `promise/future` 迁移到 `awaitable`。

**Architecture:** 头文件库。新增 `include/coro/awaitable.h`,提供值-或-关闭的 `result<T>`、可复用的 `awaitable<T>`(`await`/`resolve`/`close`)及其 `void` 特化,与异常类型 `awaitable_closed`。`unbuffered_channel` 的 rendezvous 语义在单线程泵模型下成立:信号能触发时消费者必已挂起在 `pop()`。信号适配器仅替换数据载体,对外返回类型与现有 API/测试完全不变。

**Tech Stack:** C++17、Boost.Fiber 1.74(`unbuffered_channel`、`channel_op_status`)、Qt5(QObject 信号连接)、QtTest、CMake。

**关联设计:** `docs/superpowers/specs/2026-06-16-awaitable-design.md`

**运行约定:** 测试二进制需 Boost 运行库路径:
`LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib`
配置已存在于 `build/`;若缺失先 `cmake -S . -B build`。

---

## File Structure

- **Create** `include/coro/awaitable.h` — `awaitable_closed` 异常、`result<T>`/`result<void>`、`awaitable<T>`/`awaitable<void>`。
- **Create** `tests/test_awaitable.cpp` — `awaitable`/`result` 单元与运行时测试。
- **Modify** `tests/CMakeLists.txt` — 注册 `test_awaitable`。
- **Modify** `include/coro/coro.h` — 聚合 `coro/awaitable.h`。
- **Modify** `include/coro/signal.h` — 迁移到 `awaitable`(替换 `promise/future`,新增关闭守卫)。
- **Modify** `tests/test_signal.cpp` — 新增「对象销毁 → 抛 `awaitable_closed`」回归用例。
- **Modify** `CHANGELOG.md` — 记录本次变更。

---

### Task 1: `result<T>` + `result<void>` + `awaitable_closed`(脚手架)

**Files:**
- Create: `include/coro/awaitable.h`
- Create: `tests/test_awaitable.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/coro/coro.h`

- [ ] **Step 1: 注册测试目标**

在 `tests/CMakeLists.txt` 的 `add_qfcoro_test(test_signal)` 之后新增一行:

```cmake
add_qfcoro_test(test_awaitable)
```

- [ ] **Step 2: 写失败测试(`result` 的值/关闭语义)**

创建 `tests/test_awaitable.cpp`:

```cpp
#include <QtTest>
#include <coro/awaitable.h>

class TestAwaitable : public QObject {
    Q_OBJECT
private slots:
    void resultValueAndClosed() {
        auto v = coro::result<int>::value(5);
        QVERIFY(v.has_value());
        QVERIFY(!v.closed());
        QVERIFY(static_cast<bool>(v));
        QCOMPARE(v.value(), 5);

        auto c = coro::result<int>::closed_();
        QVERIFY(!c.has_value());
        QVERIFY(c.closed());
        QVERIFY(!static_cast<bool>(c));

        auto vv = coro::result<void>::value();
        QVERIFY(vv.has_value());
        auto vc = coro::result<void>::closed_();
        QVERIFY(vc.closed());
    }
};

QTEST_GUILESS_MAIN(TestAwaitable)
#include "test_awaitable.moc"
```

- [ ] **Step 3: 运行测试,确认失败**

Run: `cmake --build build -j --target test_awaitable`
Expected: FAIL —— 编译错误,`coro/awaitable.h` 不存在。

- [ ] **Step 4: 实现 `awaitable.h`(本任务只需 `result` 与异常;`awaitable<T>` 在下一任务补全,但本步直接写入完整文件以避免后续重复编辑)**

创建 `include/coro/awaitable.h`:

```cpp
#pragma once
#include <optional>
#include <stdexcept>
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

    T&        value() &       { return *v_; }
    T&&       value() &&      { return std::move(*v_); }
    const T&  value() const&  { return *v_; }
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
```

- [ ] **Step 5: 在伞头文件聚合**

修改 `include/coro/coro.h`,在 `#include "coro/core.h"` 之后新增一行:

```cpp
#include "coro/awaitable.h"
```

- [ ] **Step 6: 运行测试,确认通过**

Run: `cmake --build build -j --target test_awaitable && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_awaitable resultValueAndClosed`
Expected: PASS(`Totals: 1 passed, 0 failed`)。

- [ ] **Step 7: 提交**

```bash
git add include/coro/awaitable.h include/coro/coro.h tests/test_awaitable.cpp tests/CMakeLists.txt
git commit -m "feat: 新增 result<T> 与 awaitable<T> 骨架(unbuffered_channel)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `awaitable<T>` 基本 resolve/await

**Files:**
- Modify: `tests/test_awaitable.cpp`

- [ ] **Step 1: 写失败测试**

在 `TestAwaitable` 的 `private slots:` 内,`resultValueAndClosed()` 之后新增。注意需要 `#include <coro/core.h>`(放在文件顶部 `#include <coro/awaitable.h>` 之后):

```cpp
    void basicResolveAwait() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        int got = -1;
        coro::launch([&, aw]{
            auto r = aw->await();
            QVERIFY(r.has_value());
            got = r.value();
            coro::quit();
        });
        coro::launch([aw]{ aw->resolve(42); });
        coro::exec();
        QCOMPARE(got, 42);
    }
```

并在文件顶部补充包含:

```cpp
#include <coro/core.h>
#include <memory>
```

- [ ] **Step 2: 运行,确认失败前先确认它能编译并跑过**

Run: `cmake --build build -j --target test_awaitable && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_awaitable basicResolveAwait`
Expected: PASS。`awaitable<T>` 实现已在 Task 1 写入,本任务用测试锁定其行为。若 FAIL,先修 `awaitable.h` 再继续。

说明:消费者 `await()` 先挂起在 `pop()`,生产者 `resolve()` 的 `push()` 与之 rendezvous;两个 `launch` 顺序无关(rendezvous 双向同步)。

- [ ] **Step 3: 提交**

```bash
git add tests/test_awaitable.cpp
git commit -m "test: awaitable<T> 基本 resolve/await 同步

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `awaitable<T>` 多次复用(流)

**Files:**
- Modify: `tests/test_awaitable.cpp`

- [ ] **Step 1: 写测试**

在 `basicResolveAwait()` 之后新增:

```cpp
    void reuseMultiShot() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        std::vector<int> got;
        coro::launch([&, aw]{
            for (int i = 0; i < 3; ++i) {
                auto r = aw->await();
                got.push_back(r.value());
            }
            coro::quit();
        });
        coro::launch([aw]{
            for (int i = 0; i < 3; ++i) aw->resolve(i * 10);
        });
        coro::exec();
        QCOMPARE(got.size(), std::size_t(3));
        QCOMPARE(got[0], 0);
        QCOMPARE(got[1], 10);
        QCOMPARE(got[2], 20);
    }
```

文件顶部补充 `#include <vector>`。

- [ ] **Step 2: 运行,确认通过**

Run: `cmake --build build -j --target test_awaitable && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_awaitable reuseMultiShot`
Expected: PASS。每次 `push` 与消费者重新武装的 `pop` rendezvous,顺序为 0/10/20。

- [ ] **Step 3: 提交**

```bash
git add tests/test_awaitable.cpp
git commit -m "test: awaitable<T> 多次复用(数据流)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `awaitable<void>` 特化

**Files:**
- Modify: `tests/test_awaitable.cpp`

- [ ] **Step 1: 写测试**

在 `reuseMultiShot()` 之后新增:

```cpp
    void voidSpecialization() {
        auto aw = std::make_shared<coro::awaitable<void>>();
        bool woke = false;
        coro::launch([&, aw]{
            auto r = aw->await();
            woke = r.has_value();
            coro::quit();
        });
        coro::launch([aw]{ aw->resolve(); });
        coro::exec();
        QVERIFY(woke);
    }
```

- [ ] **Step 2: 运行,确认通过**

Run: `cmake --build build -j --target test_awaitable && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_awaitable voidSpecialization`
Expected: PASS。

- [ ] **Step 3: 提交**

```bash
git add tests/test_awaitable.cpp
git commit -m "test: awaitable<void> 特化

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: `close()` 语义(唤醒等待者 + resolve 失败)

**Files:**
- Modify: `tests/test_awaitable.cpp`

- [ ] **Step 1: 写测试**

在 `voidSpecialization()` 之后新增两个用例:

```cpp
    void closeWakesAwaiter() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        bool gotClosed = false;
        coro::launch([&, aw]{
            auto r = aw->await();
            gotClosed = r.closed();
            coro::quit();
        });
        coro::launch([aw]{ aw->close(); });
        coro::exec();
        QVERIFY(gotClosed);
    }

    void resolveAfterCloseFails() {
        auto aw = std::make_shared<coro::awaitable<int>>();
        bool resolveOk = true;
        coro::launch([&, aw]{
            aw->close();
            resolveOk = aw->resolve(1);   // 关闭后提交 → false
            coro::quit();
        });
        coro::exec();
        QVERIFY(!resolveOk);
    }
```

- [ ] **Step 2: 运行,确认通过**

Run: `cmake --build build -j --target test_awaitable && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_awaitable`
Expected: PASS(全部 6 个用例)。

- [ ] **Step 3: 提交**

```bash
git add tests/test_awaitable.cpp
git commit -m "test: awaitable close() 唤醒等待者并使后续 resolve 失败

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: 迁移信号适配器到 `awaitable`

**Files:**
- Modify: `include/coro/signal.h`(整体重写 detail 实现)
- Modify: `tests/test_signal.cpp`(新增对象销毁回归用例)

- [ ] **Step 1: 先加回归测试(对象销毁 → 抛 `awaitable_closed`)**

在 `tests/test_signal.cpp` 的 `TestSignal` 末尾(`awaitConvertedTyped()` 之后)新增。文件顶部已 `#include <coro/signal.h>`,`coro::awaitable_closed` 经此可见:

```cpp
    // 等待期间对象被销毁:连接 lambda 析构 → awaitable 关闭 → await 抛 awaitable_closed
    void closedThrowsOnDestroy() {
        bool threw = false;
        auto* e = new Emitter;
        coro::launch([&]{
            try { coro::await(e, &Emitter::oneArg); }
            catch (const coro::awaitable_closed&) { threw = true; }
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ delete e; });
        coro::exec();
        QVERIFY(threw);
    }
```

- [ ] **Step 2: 运行,确认失败**

Run: `cmake --build build -j --target test_signal && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_signal closedThrowsOnDestroy`
Expected: FAIL —— 旧实现 `coro::awaitable_closed` 未定义(编译错误)或对象销毁后协程永久挂起(超时/未抛)。

- [ ] **Step 3: 重写 `include/coro/signal.h`**

整体替换为(以 `awaitable` 取代 `promise/future`,新增关闭守卫;`pack_result`/`signal_args` 不变;`set_typed` → 返回值的 `make_typed`):

```cpp
#pragma once
#include <tuple>
#include <type_traits>
#include <memory>
#include <utility>
#include <QObject>
#include <boost/fiber/all.hpp>
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

    auto r = aw->await();
    if (r.closed()) throw awaitable_closed("coro::await(signal): awaitable closed");
    if constexpr (std::is_void_v<R>) { return; }
    else { return std::move(r).value(); }
}

// 用接收到的信号前 K 个参数构造结果(各自转换为对应 Want 形参类型)
template<class R, class... Want, class Tuple, std::size_t... I>
R make_typed(Tuple& t, std::index_sequence<I...>) {
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
template<class W0, class... Wr, class Obj, class Sig>
auto await(Obj* obj, Sig sig) {
    return detail::await_typed_impl(
        obj, sig,
        static_cast<std::tuple<W0, Wr...>*>(nullptr),
        static_cast<typename detail::signal_args<Sig>::type*>(nullptr));
}

} // namespace coro
```

- [ ] **Step 4: 运行新回归用例,确认通过**

Run: `cmake --build build -j --target test_signal && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_signal closedThrowsOnDestroy`
Expected: PASS。

- [ ] **Step 5: 运行整套 test_signal,确认无回归**

Run: `cmake --build build -j --target test_signal && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ./build/tests/test_signal`
Expected: PASS(原 7 个 + 新增 1 个 = 8 个用例全过)。

- [ ] **Step 6: 提交**

```bash
git add include/coro/signal.h tests/test_signal.cpp
git commit -m "feat: 信号 await 改用 awaitable<T> 传递数据(取代 promise/future)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: 全量回归 + CHANGELOG + 推送

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: 全量构建并跑全部测试**

Run: `cmake --build build -j && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib ctest --test-dir build --output-on-failure`
Expected: 全部测试套件 PASS(原 9 套 + 新增 `test_awaitable` = 10 套)。

- [ ] **Step 2: 更新 CHANGELOG**

在 `CHANGELOG.md` 的 `## [Unreleased]` 下、现有首个 `### Added` 之前,插入一节:

```markdown
### Added — awaitable<T> 异步数据同步原语

- **`coro::awaitable<T>`**(`coro/awaitable.h`,已并入伞头文件):基于
  `boost::fibers::unbuffered_channel` 的**可复用** rendezvous 原语。`await()` 取一个值返回
  `coro::result<T>`(值-或-关闭);`resolve(v)` 提交数据(rendezvous,关闭后返回 `false`);
  `close()` 关闭通道并唤醒等待者。含 `awaitable<void>` 特化与异常类型 `coro::awaitable_closed`。
- **信号 await 迁移**:`coro::await(obj, signal)` / `coro::await<Types...>(...)` 底层由
  `promise/future` 改为 `awaitable<T>` 传递数据;对外返回类型与行为不变。连接 lambda 持有
  关闭守卫,对象销毁/断连时关闭 awaitable,使等待中的 `await` 抛 `awaitable_closed`
  (复刻旧版 `broken_promise`)。
- 新增测试 `test_awaitable`(基本同步/多次复用/void 特化/close 语义);`test_signal`
  新增「对象销毁 → 抛 `awaitable_closed`」回归。
```

- [ ] **Step 3: 提交 CHANGELOG**

```bash
git add CHANGELOG.md
git commit -m "docs: CHANGELOG 记录 awaitable<T> 与信号 await 迁移

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 4: 推送到 origin**

(用户标准工作流:每次变更后推送到 GitHub origin。)

Run: `git push origin master`
Expected: 推送成功。

---

## Notes

- `unbuffered_channel::pop(T&)` 要求 `T` 可默认构造;信号实参经 `std::decay_t` 后均满足,`void` 路径用 `std::monostate`。
- 关闭守卫用 `std::shared_ptr<void>` + 自定义删除器:连接 lambda 是其唯一持有者,lambda 析构即触发 `close()`。
- 本轮仅迁移信号适配器;future/iodevice/process/network 维持现有 promise/future 实现。
