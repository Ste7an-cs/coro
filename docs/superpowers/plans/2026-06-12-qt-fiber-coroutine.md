# Qt + Boost.Fiber 协程库(qfcoro)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 Boost.Fiber(有栈协程)在单线程"泵驱动"模型下,为 Qt 的信号、定时器、QFuture、QIODevice 提供"看起来同步"的协程接口,并提供 `Task<T>` 组合层。

**Architecture:** `coro::exec()` 取代 `app.exec()`,主循环交替 `boost::this_fiber::yield()`(轮转就绪协程)与 `QCoreApplication::processEvents(WaitForMoreEvents)`(阻塞等 Qt 事件)。协程只通过 `coro::await*` 经 `boost::fibers::future::get()` 挂起;Qt 信号在 `processEvents` 内触发并 `set_value` 唤醒对应 fiber。不修改 Boost.Fiber 默认调度器。

**Tech Stack:** C++17,Boost.Fiber/Context 1.74,Qt 5.15(Core/Test/Concurrent),CMake,Qt Test 框架,TDD。

**关键路径(本机已确认):**
- Boost 安装根:`/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1`(其 `lib/cmake/` 提供 `Boost-1.74.0`、`boost_fiber-1.74.0`、`boost_context-1.74.0` 的 config)
- 运行期需要该 Boost 的 `lib` 在 `LD_LIBRARY_PATH` 中(libboost_fiber/context 为动态库)

**全局约定:**
- 每个测试可执行文件用 `QTEST_GUILESS_MAIN(Class)`(自动创建 `QCoreApplication`,在测试槽内调用 `coro::exec()`)。
- 含 `Q_OBJECT` 的测试类放在 `.cpp` 内,文件末尾 `#include "<文件名去扩展>.moc"`,由 AUTOMOC 处理。
- 每个测试用 `QTimer::singleShot` 触发外部事件、并在协程内调用 `coro::quit()` 结束 `exec()`。
- 运行测试统一命令(含 Boost 运行期库路径):
  ```bash
  cd /home/david/zpj/coro/build && \
  LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
  ctest --output-on-failure
  ```

## 文件结构

```
coro/
  CMakeLists.txt
  include/coro/
    coro.h        # 伞头文件,聚合下列所有
    core.h        # exec/quit/launch/yield/sleep 声明
    signal.h      # await(QObject*, signal) + 信号参数萃取(header-only)
    task.h        # Task<T>、async、await(Task)(header-only)
    when.h        # whenAll / whenAny(header-only)
    future.h      # await(QFuture<T>) / await(QFuture<void>)(header-only)
    iodevice.h    # await(QIODevice*)(header-only)
  src/
    core.cpp      # 驱动循环 + launch/yield/sleep 实现(唯一 .cpp)
  examples/
    basic_signal.cpp
    future_demo.cpp
  tests/
    CMakeLists.txt
    test_driver.cpp
    test_sleep.cpp
    test_signal.cpp
    test_task.cpp
    test_when.cpp
    test_future.cpp
    test_iodevice.cpp
  README.md
```

职责边界:`core.cpp` 是唯一编译单元,封装与 Boost.Fiber 调度器/Qt 事件循环的全部交互;其余适配器是 header-only 模板,各自只负责一类 Qt 对象的 await。

---

### Task 1: CMake 骨架 + 链接冒烟测试

**Files:**
- Create: `CMakeLists.txt`
- Create: `tests/CMakeLists.txt`
- Create: `tests/test_driver.cpp`
- Create: `include/coro/core.h`
- Create: `src/core.cpp`

- [ ] **Step 1: 写最小 core.h（仅声明，便于先建库目标）**

`include/coro/core.h`:
```cpp
#pragma once
#include <functional>
#include <chrono>

namespace coro {
int  exec();
void quit(int code = 0);
void launch(std::function<void()> fn);
void yield();
void sleep(std::chrono::milliseconds ms);
}
```

- [ ] **Step 2: 写最小 core.cpp（占位实现，保证可链接）**

`src/core.cpp`:
```cpp
#include "coro/core.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QAbstractEventDispatcher>
#include <boost/fiber/all.hpp>
#include <memory>

namespace {
struct DriverState { bool running = false; int code = 0; };
DriverState& state() { static DriverState s; return s; }
}

namespace coro {

int exec() {
    auto& s = state();
    s.running = true;
    s.code = 0;
    while (s.running) {
        boost::this_fiber::yield();
        if (!s.running) break;
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents | QEventLoop::AllEvents);
    }
    return s.code;
}

void quit(int code) {
    auto& s = state();
    s.code = code;
    s.running = false;
    if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
}

void launch(std::function<void()> fn) {
    boost::fibers::fiber(std::move(fn)).detach();
}

void yield() {
    if (auto* d = QCoreApplication::eventDispatcher()) d->wakeUp();
    boost::this_fiber::yield();
}

void sleep(std::chrono::milliseconds ms) {
    boost::fibers::promise<void> p;
    auto f = p.get_future();
    auto pp = std::make_shared<boost::fibers::promise<void>>(std::move(p));
    QTimer::singleShot(static_cast<int>(ms.count()), [pp]{ pp->set_value(); });
    f.get();
}

}
```

- [ ] **Step 3: 写顶层 CMakeLists.txt**

`CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)
project(qfcoro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 指向本机自定义 Boost
list(APPEND CMAKE_PREFIX_PATH
  "/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1")

find_package(Boost 1.74 CONFIG REQUIRED COMPONENTS fiber context)
find_package(Qt5 REQUIRED COMPONENTS Core Test Concurrent)

add_library(qfcoro src/core.cpp)
target_include_directories(qfcoro PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qfcoro PUBLIC Qt5::Core Boost::fiber Boost::context)

enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 4: 写 tests/CMakeLists.txt（带可复用 helper）**

`tests/CMakeLists.txt`:
```cmake
function(add_qfcoro_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE qfcoro Qt5::Core Qt5::Test Qt5::Concurrent)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_qfcoro_test(test_driver)
```

- [ ] **Step 5: 写冒烟测试（验证 boost_fiber 链接 + 运行）**

`tests/test_driver.cpp`:
```cpp
#include <QtTest>
#include <boost/fiber/all.hpp>

class TestDriver : public QObject {
    Q_OBJECT
private slots:
    void boostFiberLinks() {
        auto f = boost::fibers::async([]{ return 42; });
        QCOMPARE(f.get(), 42);
    }
};

QTEST_GUILESS_MAIN(TestDriver)
#include "test_driver.moc"
```

- [ ] **Step 6: 配置并构建**

Run:
```bash
cd /home/david/zpj/coro && cmake -S . -B build && cmake --build build -j
```
Expected: 配置与编译成功,生成 `build/tests/test_driver`。

- [ ] **Step 7: 运行冒烟测试**

Run:
```bash
cd /home/david/zpj/coro/build && \
LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest --output-on-failure
```
Expected: PASS（`1 tests passed`，boostFiberLinks 通过）。

- [ ] **Step 8: 提交**

```bash
cd /home/david/zpj/coro && git add CMakeLists.txt tests include src && \
git commit -m "build: CMake 骨架 + boost_fiber 链接冒烟测试"
```

---

### Task 2: 驱动循环 — launch 在 exec 中运行并能 quit

验证 Task 1 已写好的 `exec/launch/quit` 实际能驱动协程。

**Files:**
- Modify: `tests/test_driver.cpp`（新增用例）
- Modify: `tests/CMakeLists.txt`（已含 test_driver，无需改）

- [ ] **Step 1: 写失败测试（协程在 exec 内运行并退出）**

在 `tests/test_driver.cpp` 的 `private slots:` 内,`boostFiberLinks` 之后新增:
```cpp
    void launchRunsAndQuits() {
        bool ran = false;
        int order = 0;
        coro::launch([&]{
            ran = true;
            order = 1;
            coro::quit(7);
        });
        int rc = coro::exec();
        QVERIFY(ran);
        QCOMPARE(order, 1);
        QCOMPARE(rc, 7);
    }
```
并在文件顶部 `#include <QtTest>` 后增加:
```cpp
#include <coro/core.h>
```

- [ ] **Step 2: 运行测试，确认通过（实现已在 Task 1 写好）**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_driver --output-on-failure
```
Expected: PASS（2 个用例）。

说明:`coro::launch` 创建的 detached fiber 在 `exec()` 第一次 `yield()` 时运行,设置标志并 `quit()`；`yield()` 返回后 `if(!running) break` 跳出循环,故不会卡在 `processEvents`。

- [ ] **Step 3: 提交**

```bash
cd /home/david/zpj/coro && git add tests/test_driver.cpp && \
git commit -m "test: 驱动循环可运行协程并响应 quit"
```

---

### Task 3: sleep — 挂起 fiber 不阻塞其他协程

**Files:**
- Create: `tests/test_sleep.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 注册测试目标**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_sleep)
```

- [ ] **Step 2: 写失败测试（两个协程并发 sleep，交错完成证明不阻塞）**

`tests/test_sleep.cpp`:
```cpp
#include <QtTest>
#include <QElapsedTimer>
#include <coro/core.h>
#include <vector>

class TestSleep : public QObject {
    Q_OBJECT
private slots:
    void concurrentSleepInterleaves() {
        std::vector<int> finished;
        int remaining = 2;
        auto done = [&]{ if (--remaining == 0) coro::quit(); };

        // 协程 A：睡 60ms 后记录 1
        coro::launch([&]{ coro::sleep(std::chrono::milliseconds(60)); finished.push_back(1); done(); });
        // 协程 B：睡 20ms 后记录 2（应先于 A 完成 → 证明 A 的 sleep 没阻塞 B）
        coro::launch([&]{ coro::sleep(std::chrono::milliseconds(20)); finished.push_back(2); done(); });

        QElapsedTimer t; t.start();
        coro::exec();

        QCOMPARE(finished.size(), size_t(2));
        QCOMPARE(finished[0], 2);   // B 先完成
        QCOMPARE(finished[1], 1);   // A 后完成
        // 并发：总耗时约 60ms 量级，远小于串行的 80ms
        QVERIFY(t.elapsed() < 200);
    }
};

QTEST_GUILESS_MAIN(TestSleep)
#include "test_sleep.moc"
```

- [ ] **Step 3: 构建并运行，确认通过（sleep 实现已在 Task 1 写好）**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_sleep --output-on-failure
```
Expected: PASS。若两协程未交错(finished[0]!=2),说明 sleep 误阻塞了线程,需检查 `sleep` 是否用 `future.get()` 挂起而非线程级阻塞。

- [ ] **Step 4: 提交**

```bash
cd /home/david/zpj/coro && git add tests/CMakeLists.txt tests/test_sleep.cpp && \
git commit -m "test: sleep 并发挂起不阻塞其他协程"
```

---

### Task 4: await(QObject*, signal) — 通用信号 await

**Files:**
- Create: `include/coro/signal.h`
- Create: `tests/test_signal.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试（先注册目标 + 测试，signal.h 尚不存在 → 编译失败）**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_signal)
```

`tests/test_signal.cpp`:
```cpp
#include <QtTest>
#include <QTimer>
#include <coro/core.h>
#include <coro/signal.h>

class Emitter : public QObject {
    Q_OBJECT
public:
signals:
    void noArg();
    void oneArg(int v);
    void twoArg(int a, const QString& b);
};

class TestSignal : public QObject {
    Q_OBJECT
private slots:
    void awaitOneArg() {
        Emitter e;
        int got = -1;
        coro::launch([&]{ got = coro::await(&e, &Emitter::oneArg); coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.oneArg(7); });
        coro::exec();
        QCOMPARE(got, 7);
    }

    void awaitNoArg() {
        Emitter e;
        bool woke = false;
        coro::launch([&]{ coro::await(&e, &Emitter::noArg); woke = true; coro::quit(); });
        QTimer::singleShot(10, [&]{ emit e.noArg(); });
        coro::exec();
        QVERIFY(woke);
    }

    void awaitTwoArgs() {
        Emitter e;
        int a = -1; QString b;
        coro::launch([&]{
            auto t = coro::await(&e, &Emitter::twoArg);
            a = std::get<0>(t); b = std::get<1>(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.twoArg(5, QStringLiteral("hi")); });
        coro::exec();
        QCOMPARE(a, 5);
        QCOMPARE(b, QStringLiteral("hi"));
    }
};

QTEST_GUILESS_MAIN(TestSignal)
#include "test_signal.moc"
```

- [ ] **Step 2: 运行测试，确认编译失败**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`coro/signal.h: No such file` 或 `coro::await` 未声明）。

- [ ] **Step 3: 实现 signal.h**

`include/coro/signal.h`:
```cpp
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
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_signal --output-on-failure
```
Expected: PASS（3 个用例）。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/signal.h tests/test_signal.cpp tests/CMakeLists.txt && \
git commit -m "feat: await(QObject*, signal) 通用信号协程"
```

---

### Task 5: Task<T> / async / await(Task) + 异常传播

**Files:**
- Create: `include/coro/task.h`
- Create: `tests/test_task.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 注册测试目标 + 写失败测试**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_task)
```

`tests/test_task.cpp`:
```cpp
#include <QtTest>
#include <QTimer>
#include <stdexcept>
#include <coro/core.h>
#include <coro/signal.h>
#include <coro/task.h>

class Emitter2 : public QObject {
    Q_OBJECT
public:
signals:
    void value(int v);
};

class TestTask : public QObject {
    Q_OBJECT
private slots:
    void asyncReturnsValue() {
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([]{ return 21 * 2; });
            got = coro::await(t);
            coro::quit();
        });
        coro::exec();
        QCOMPARE(got, 42);
    }

    void awaitTaskWaitsForSignal() {
        Emitter2 e;
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([&]{ return coro::await(&e, &Emitter2::value); });
            got = coro::await(t);
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ emit e.value(99); });
        coro::exec();
        QCOMPARE(got, 99);
    }

    void exceptionPropagatesThroughTask() {
        bool caught = false;
        coro::launch([&]{
            coro::Task<int> t = coro::async([]() -> int { throw std::runtime_error("boom"); });
            try { coro::await(t); } catch (const std::runtime_error&) { caught = true; }
            coro::quit();
        });
        coro::exec();
        QVERIFY(caught);
    }
};

QTEST_GUILESS_MAIN(TestTask)
#include "test_task.moc"
```

- [ ] **Step 2: 运行，确认编译失败**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`coro/task.h: No such file`）。

- [ ] **Step 3: 实现 task.h**

`include/coro/task.h`:
```cpp
#pragma once
#include <boost/fiber/all.hpp>
#include <chrono>
#include <type_traits>
#include <utility>

namespace coro {

template<class T>
class Task {
public:
    Task() = default;
    explicit Task(boost::fibers::future<T> f) : fut_(std::move(f)) {}
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 等待并取结果（异常重抛）
    T get() { return fut_.get(); }

    // 仅等待完成，不消费结果（whenAny 轮询用不到，这里供组合层）
    void wait() { fut_.wait(); }

    // 非消费式：是否已就绪
    bool done() const {
        return fut_.valid() &&
               fut_.wait_for(std::chrono::seconds(0)) == boost::fibers::future_status::ready;
    }

    // 非阻塞续接：起一个 watcher fiber，完成后回调（消费本 Task）
    template<class F>
    void then(F cb) {
        boost::fibers::fiber([f = std::move(fut_), cb = std::move(cb)]() mutable {
            if constexpr (std::is_void_v<T>) { f.get(); cb(); }
            else                            { cb(f.get()); }
        }).detach();
    }

private:
    boost::fibers::future<T> fut_;
};

// 启动协程并返回句柄
template<class Fn>
auto async(Fn fn) -> Task<std::invoke_result_t<Fn>> {
    using T = std::invoke_result_t<Fn>;
    return Task<T>(boost::fibers::async(std::move(fn)));
}

// 等另一个 Task 完成取结果
template<class T>
T await(Task<T>& t) { return t.get(); }

} // namespace coro
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_task --output-on-failure
```
Expected: PASS（3 个用例）。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/task.h tests/test_task.cpp tests/CMakeLists.txt && \
git commit -m "feat: Task<T>/async/await(Task) 与异常传播"
```

---

### Task 6: Task::then — 非阻塞续接

**Files:**
- Modify: `tests/test_task.cpp`（新增用例）

- [ ] **Step 1: 新增失败测试**

在 `tests/test_task.cpp` 的 `private slots:` 内追加用例:
```cpp
    void thenInvokesCallbackWithResult() {
        Emitter2 e;
        int got = 0;
        coro::launch([&]{
            coro::Task<int> t = coro::async([&]{ return coro::await(&e, &Emitter2::value); });
            t.then([&](int v){ got = v; coro::quit(); });
            // 注意：launch 体在此返回，then 的 watcher fiber 负责后续
        });
        QTimer::singleShot(10, [&]{ emit e.value(123); });
        coro::exec();
        QCOMPARE(got, 123);
    }
```

- [ ] **Step 2: 构建并运行，确认通过（then 已在 Task 5 实现）**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_task --output-on-failure
```
Expected: PASS（4 个用例）。

- [ ] **Step 3: 提交**

```bash
cd /home/david/zpj/coro && git add tests/test_task.cpp && \
git commit -m "test: Task::then 非阻塞续接"
```

---

### Task 7: whenAll — 并发等全部

**Files:**
- Create: `include/coro/when.h`
- Create: `tests/test_when.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 注册目标 + 写失败测试**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_when)
```

`tests/test_when.cpp`:
```cpp
#include <QtTest>
#include <QElapsedTimer>
#include <coro/core.h>
#include <coro/task.h>
#include <coro/when.h>

class TestWhen : public QObject {
    Q_OBJECT
private slots:
    void whenAllReturnsAllResults() {
        int a = 0, b = 0;
        QElapsedTimer timer; timer.start();
        coro::launch([&]{
            coro::Task<int> ta = coro::async([]{ coro::sleep(std::chrono::milliseconds(50)); return 1; });
            coro::Task<int> tb = coro::async([]{ coro::sleep(std::chrono::milliseconds(50)); return 2; });
            auto [ra, rb] = coro::whenAll(ta, tb);
            a = ra; b = rb;
            coro::quit();
        });
        coro::exec();
        QCOMPARE(a, 1);
        QCOMPARE(b, 2);
        QVERIFY(timer.elapsed() < 150);   // 并发：约 50ms，而非串行 100ms
    }
};

QTEST_GUILESS_MAIN(TestWhen)
#include "test_when.moc"
```

- [ ] **Step 2: 运行，确认编译失败**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`coro/when.h: No such file`）。

- [ ] **Step 3: 实现 when.h（先只写 whenAll）**

`include/coro/when.h`:
```cpp
#pragma once
#include <tuple>
#include <cstddef>
#include "coro/task.h"
#include "coro/core.h"

namespace coro {

// 并发等待全部 Task 完成，返回结果 tuple。
// 约束：所有 Task 的结果类型必须为非 void（如需 void 用占位返回值包装）。
template<class... Ts>
std::tuple<Ts...> whenAll(Task<Ts>&... ts) {
    // 各 Task 已各自在独立 fiber 并发运行；顺序 get() 仍只等到"最慢"那个完成。
    return std::tuple<Ts...>(ts.get()...);
}

} // namespace coro
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_when --output-on-failure
```
Expected: PASS。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/when.h tests/test_when.cpp tests/CMakeLists.txt && \
git commit -m "feat: whenAll 并发等待全部 Task"
```

---

### Task 8: whenAny — 等第一个完成

**Files:**
- Modify: `include/coro/when.h`
- Modify: `tests/test_when.cpp`

- [ ] **Step 1: 新增失败测试**

在 `tests/test_when.cpp` 的 `private slots:` 内追加:
```cpp
    void whenAnyReturnsFirstIndex() {
        size_t idx = SIZE_MAX;
        int winnerVal = 0;
        coro::launch([&]{
            coro::Task<int> slow = coro::async([]{ coro::sleep(std::chrono::milliseconds(80)); return 10; });
            coro::Task<int> fast = coro::async([]{ coro::sleep(std::chrono::milliseconds(20)); return 20; });
            idx = coro::whenAny(slow, fast);
            // 胜者结果仍可取（whenAny 不消费 future）
            winnerVal = (idx == 1) ? fast.get() : slow.get();
            coro::quit();
        });
        coro::exec();
        QCOMPARE(idx, size_t(1));      // fast 先完成
        QCOMPARE(winnerVal, 20);
    }
```

- [ ] **Step 2: 运行，确认编译失败（whenAny 未定义）**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`whenAny` 不是 `coro` 的成员）。

- [ ] **Step 3: 在 when.h 增加 whenAny（轮询式，避免对同一 future 双等待）**

在 `include/coro/when.h` 的 `whenAll` 之后、`} // namespace coro` 之前插入:
```cpp
// 等待第一个完成的 Task，返回其下标（从 0 起）。
// 采用 1ms 轮询：每轮在主协程内用非消费式 done() 检查，再 sleep 让出事件循环。
// 不消费任何 future，调用后胜者结果仍可 get()。
template<class... Ts>
std::size_t whenAny(Task<Ts>&... ts) {
    for (;;) {
        std::size_t idx = 0;
        std::size_t found = SIZE_MAX;
        auto check = [&](auto& t) {
            if (found == SIZE_MAX && t.done()) found = idx;
            ++idx;
        };
        (check(ts), ...);
        if (found != SIZE_MAX) return found;
        coro::sleep(std::chrono::milliseconds(1));   // 让出：驱动事件循环 + 其他协程
    }
}
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_when --output-on-failure
```
Expected: PASS（2 个用例）。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/when.h tests/test_when.cpp && \
git commit -m "feat: whenAny 等待第一个完成的 Task"
```

---

### Task 9: await(QFuture<T>) / await(QFuture<void>)

**Files:**
- Create: `include/coro/future.h`
- Create: `tests/test_future.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 注册目标 + 写失败测试**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_future)
```

`tests/test_future.cpp`:
```cpp
#include <QtTest>
#include <QtConcurrent>
#include <QFuture>
#include <stdexcept>
#include <coro/core.h>
#include <coro/future.h>

class TestFuture : public QObject {
    Q_OBJECT
private slots:
    void awaitFutureResult() {
        int got = 0;
        coro::launch([&]{
            QFuture<int> f = QtConcurrent::run([]{ return 21 * 2; });
            got = coro::await(f);
            coro::quit();
        });
        coro::exec();
        QCOMPARE(got, 42);
    }

    void awaitVoidFuture() {
        bool ran = false;
        coro::launch([&]{
            QFuture<void> f = QtConcurrent::run([]{ /* 副作用 */ });
            coro::await(f);
            ran = true;
            coro::quit();
        });
        coro::exec();
        QVERIFY(ran);
    }
};

QTEST_GUILESS_MAIN(TestFuture)
#include "test_future.moc"
```

- [ ] **Step 2: 运行，确认编译失败**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`coro/future.h: No such file`）。

- [ ] **Step 3: 实现 future.h**

`include/coro/future.h`:
```cpp
#pragma once
#include <QFuture>
#include <QFutureWatcher>
#include <memory>
#include <boost/fiber/all.hpp>

namespace coro {

// await 一个返回值的 QFuture<T>
template<class T>
T await(const QFuture<T>& f) {
    QFuture<T> fut = f;
    if (fut.isFinished()) return fut.result();

    QFutureWatcher<T> watcher;
    auto pr  = std::make_shared<boost::fibers::promise<void>>();
    auto pfu = pr->get_future();
    QObject::connect(&watcher, &QFutureWatcherBase::finished, [pr]{ pr->set_value(); });
    watcher.setFuture(fut);
    pfu.get();
    return fut.result();
}

// await 一个 QFuture<void>（非模板，优先于上面的模板被选中）
inline void await(const QFuture<void>& f) {
    QFuture<void> fut = f;
    if (fut.isFinished()) return;

    QFutureWatcher<void> watcher;
    auto pr  = std::make_shared<boost::fibers::promise<void>>();
    auto pfu = pr->get_future();
    QObject::connect(&watcher, &QFutureWatcherBase::finished, [pr]{ pr->set_value(); });
    watcher.setFuture(fut);
    pfu.get();
}

} // namespace coro
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_future --output-on-failure
```
Expected: PASS（2 个用例）。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/future.h tests/test_future.cpp tests/CMakeLists.txt && \
git commit -m "feat: await(QFuture<T>) 与 await(QFuture<void>)"
```

---

### Task 10: await(QIODevice*) — 等 readyRead

**Files:**
- Create: `include/coro/iodevice.h`
- Create: `tests/test_iodevice.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 注册目标 + 写失败测试**

在 `tests/CMakeLists.txt` 末尾追加:
```cmake
add_qfcoro_test(test_iodevice)
```

`tests/test_iodevice.cpp`(用 `QBuffer`,写入后会发出 `readyRead`):
```cpp
#include <QtTest>
#include <QBuffer>
#include <QTimer>
#include <coro/core.h>
#include <coro/iodevice.h>

class TestIoDevice : public QObject {
    Q_OBJECT
private slots:
    void awaitReadyRead() {
        QBuffer buf;
        buf.open(QIODevice::ReadWrite);
        bool woke = false;
        coro::launch([&]{
            coro::await(&buf);
            woke = true;
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ buf.write("hi"); });
        coro::exec();
        QVERIFY(woke);
        QCOMPARE(buf.size(), qint64(2));
    }
};

QTEST_GUILESS_MAIN(TestIoDevice)
#include "test_iodevice.moc"
```

- [ ] **Step 2: 运行，确认编译失败**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j 2>&1 | tail -5
```
Expected: FAIL（`coro/iodevice.h: No such file`）。

- [ ] **Step 3: 实现 iodevice.h**

`include/coro/iodevice.h`:
```cpp
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
```

- [ ] **Step 4: 构建并运行，确认通过**

Run:
```bash
cd /home/david/zpj/coro && cmake --build build -j && \
cd build && LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH \
ctest -R test_iodevice --output-on-failure
```
Expected: PASS。若 `QBuffer` 在本机未发 `readyRead`,改用 `QLocalServer`/`QLocalSocket` 回环(需 `Qt5::Network`)；优先按 QBuffer 方案。

- [ ] **Step 5: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/iodevice.h tests/test_iodevice.cpp tests/CMakeLists.txt && \
git commit -m "feat: await(QIODevice*) 等 readyRead"
```

---

### Task 11: 伞头文件 + 示例 + README + 全量回归

**Files:**
- Create: `include/coro/coro.h`
- Create: `examples/basic_signal.cpp`
- Create: `examples/future_demo.cpp`
- Create: `README.md`
- Modify: `CMakeLists.txt`（增加 examples 子目录）
- Create: `examples/CMakeLists.txt`

- [ ] **Step 1: 写伞头文件**

`include/coro/coro.h`:
```cpp
#pragma once
// qfcoro: 基于 Boost.Fiber 的 Qt 协程库（单线程泵驱动模型）
#include "coro/core.h"
#include "coro/signal.h"
#include "coro/task.h"
#include "coro/when.h"
#include "coro/future.h"
#include "coro/iodevice.h"
```

- [ ] **Step 2: 写示例 basic_signal.cpp**

`examples/basic_signal.cpp`:
```cpp
#include <QCoreApplication>
#include <QTimer>
#include <coro/coro.h>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    coro::launch([]{
        std::puts("协程启动，等 500ms ...");
        coro::sleep(std::chrono::milliseconds(500));
        std::puts("醒来，结束。");
        coro::quit();
    });

    return coro::exec();   // 取代 app.exec()
}
```

- [ ] **Step 3: 写示例 future_demo.cpp**

`examples/future_demo.cpp`:
```cpp
#include <QCoreApplication>
#include <QtConcurrent>
#include <coro/coro.h>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    coro::launch([]{
        coro::Task<int> a = coro::async([]{ coro::sleep(std::chrono::milliseconds(100)); return 1; });
        coro::Task<int> b = coro::async([]{ coro::sleep(std::chrono::milliseconds(100)); return 2; });
        auto [ra, rb] = coro::whenAll(a, b);
        std::printf("whenAll -> %d, %d\n", ra, rb);

        QFuture<int> f = QtConcurrent::run([]{ return 42; });
        std::printf("QFuture -> %d\n", coro::await(f));

        coro::quit();
    });

    return coro::exec();
}
```

- [ ] **Step 4: 写 examples/CMakeLists.txt 并接入顶层**

`examples/CMakeLists.txt`:
```cmake
add_executable(basic_signal basic_signal.cpp)
target_link_libraries(basic_signal PRIVATE qfcoro Qt5::Core)

add_executable(future_demo future_demo.cpp)
target_link_libraries(future_demo PRIVATE qfcoro Qt5::Core Qt5::Concurrent)
```

在顶层 `CMakeLists.txt` 末尾(`add_subdirectory(tests)` 之后)追加:
```cmake
add_subdirectory(examples)
```

- [ ] **Step 5: 写 README.md**

`README.md`:
```markdown
# qfcoro

基于 **Boost.Fiber**(有栈协程)的 Qt 协程库,仿照 QCoro 的能力,但无需 C++20 `co_await`。
采用**单线程泵驱动**模型:用 `coro::exec()` 取代 `app.exec()`。

## 构建

```bash
cmake -S . -B build && cmake --build build -j
```

依赖:Qt5(Core/Test/Concurrent)、Boost.Fiber/Context 1.74。运行期需将 Boost 的 `lib` 加入
`LD_LIBRARY_PATH`。

## 用法

```cpp
#include <coro/coro.h>

coro::launch([]{
    int v = coro::await(obj, &Obj::someSignal);   // 等信号,返回参数
    coro::sleep(std::chrono::milliseconds(100));   // 挂起,不阻塞事件循环
    int r = coro::await(QtConcurrent::run([]{ return 42; }));  // 等 QFuture
});
return coro::exec();   // 取代 app.exec()
```

### API

- `coro::exec()` / `coro::quit(code)` — 主循环。
- `coro::launch(fn)` / `coro::async(fn) -> Task<T>` — 启动协程。
- `coro::await(obj, &T::signal)` — 等信号(0→void,1→值,N→tuple)。
- `coro::sleep(ms)` — 挂起当前协程。
- `coro::await(QFuture<T>)` / `coro::await(QIODevice*)` — 适配器。
- `coro::whenAll(...)` / `coro::whenAny(...)` — 组合。
- `Task<T>::get()` / `then(cb)` / `done()` — 句柄。

## 已知限制

- 嵌套 Qt 事件循环(如 `QDialog::exec()`)期间协程不轮转。
- 所有 `coro::await*` 必须在 `coro::exec()` 所在线程的协程内调用。
- `whenAll` 要求各 Task 结果非 void。
```

- [ ] **Step 6: 重新配置（新增 examples）并全量构建**

Run:
```bash
cd /home/david/zpj/coro && cmake -S . -B build && cmake --build build -j
```
Expected: 全部编译成功,生成两个 example 可执行文件与全部测试。

- [ ] **Step 7: 跑示例 + 全量回归测试**

Run:
```bash
cd /home/david/zpj/coro/build && \
export LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib:$LD_LIBRARY_PATH && \
./examples/basic_signal && ./examples/future_demo && ctest --output-on-failure
```
Expected:
- `basic_signal` 打印启动/醒来两行。
- `future_demo` 打印 `whenAll -> 1, 2` 与 `QFuture -> 42`。
- `ctest`:全部测试 PASS（test_driver/sleep/signal/task/when/future/iodevice）。

- [ ] **Step 8: 提交**

```bash
cd /home/david/zpj/coro && git add include/coro/coro.h examples README.md CMakeLists.txt && \
git commit -m "feat: 伞头文件、示例与 README;全量回归通过"
```

---

## 自检结论(对照 spec)

- 架构(单线程泵驱动、不改调度器、挂起/恢复机制):Task 1–2 ✅
- API 风格 C(底层 await + Task 层):Task 4(await 信号)、Task 5–6(Task/async/then)、Task 7–8(when)✅
- 适配器范围(信号、sleep、QFuture、QIODevice):Task 3/4/9/10 ✅(网络/进程为非目标,未排期)
- 错误处理(异常沿 fiber 栈 + 经 Task 重抛):Task 5 ✅
- 线程模型约束:README 记录 ✅
- 工程结构/构建/Qt Test/TDD:Task 1 起逐任务 TDD ✅
- 已知限制(嵌套事件循环):README 记录 ✅

类型/签名一致性核对:`coro::await`(信号/Task/QFuture/QIODevice 重载)、`coro::async`→`Task<T>`、`Task::get/wait/done/then`、`whenAll`/`whenAny`、`coro::sleep(std::chrono::milliseconds)` 在各任务中用法一致。
