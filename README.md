# qfcoro

基于 **Boost.Fiber**(有栈协程)的 Qt 协程库,仿照 QCoro 的能力,但无需 C++20 `co_await`。
采用**单线程泵驱动**模型:用 `coro::exec()` 取代 `app.exec()`。

## 构建

```bash
cmake -S . -B build && cmake --build build -j
```

依赖:Qt5(Core/Test/Concurrent)、Boost.Fiber/Context 1.74。运行期需将 Boost 的 `lib` 加入
`LD_LIBRARY_PATH`(本机:`/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib`)。

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
- `coro::await(QProcess*)` — 等进程结束,返回退出码(`#include <coro/process.h>`,已在伞头文件中)。
- `coro::await(QNetworkReply*)` — 等网络请求完成(**opt-in**:`#include <coro/network.h>` 且链接 `Qt5::Network`,不在伞头文件中)。
- `coro::whenAll(...)` / `coro::whenAny(...)` — 组合。
- `Task<T>::get()` / `then(cb)` / `done()` / `wait()` — 句柄。

> `coro/coro.h` 伞头文件聚合除网络外的全部适配器。`await(QNetworkReply*)` 因依赖 `Qt5::Network`
> 设计为可选:需显式 `#include <coro/network.h>` 并在 CMake 中链接 `Qt5::Network`。

## 已知限制

- **嵌套 Qt 事件循环**(如 `QDialog::exec()`)期间协程不轮转。
- 所有 `coro::await*` / `launch` / `async` / `sleep` 必须在 `coro::exec()` 所在线程的协程上下文中调用。
- `whenAll` 要求各 Task 结果类型非 void。
- **单次语义、无取消/超时**:`await(signal)` 与 `await(QIODevice*)` 假定信号/`readyRead` 终会触发。若对象在等待期间被销毁、或信号永不触发,协程将被永久挂起(并可能抛出 `broken_promise`);若数据在 `await` 之前已到达,该次边沿可能被错过。本版本不提供取消或超时。
- `await(Task<T>&)` 是单次消费:对同一 Task 重复 `await`/`get` 会抛 `future_already_retrieved`。
- **退出前请确保协程完成**:`coro::quit()` 时仍处于挂起的协程会被丢弃;阻塞在 future 上的 detached fiber 可能在进程退出(boost.fibers 静态调度器析构)时空转。退出前应让相关协程跑完(必要时用 `Task::wait()` 排空)。
