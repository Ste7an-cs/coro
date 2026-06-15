# 设计文档:基于 Boost.Fiber 的 Qt 协程库(qfcoro)

- 日期:2026-06-12
- 状态:已确认设计,待 review 后进入实现计划
- 目标定位:**可用的生产库**

## 1. 背景与目标

仿照 [QCoro](https://github.com/qcoro/qcoro)(C++20 协程 + Qt),但**底层用 Boost.Fiber(有栈协程)替代 C++20 无栈协程**,为 Qt 常用异步操作提供"看起来同步"的协程接口。

约束:
- **不自定义/不修改 Boost.Fiber 调度器**,使用默认 `round_robin` 调度器。
- 单线程模型:Qt 事件循环与 fiber 调度器同处主线程,协程可直接操作任意 QObject/widget(体验对齐 QCoro)。

环境(已确认):
- Boost 1.74,含 `libboost_fiber` / `libboost_context`,路径 `/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1`
- Qt 5.15.3(`/usr/lib/x86_64-linux-gnu`)
- g++ 11.4,CMake 3.29

## 2. 总体架构:单线程 · 泵驱动(Pump-driver)

用 `coro::exec()` 取代 `app.exec()` 作为程序主循环:

```cpp
// coro::exec() 核心(简化)
int exec() {
    while (running) {
        boost::this_fiber::yield();                          // 轮转所有就绪协程 fiber
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents
                                        | QEventLoop::AllEvents); // 阻塞等 Qt 事件
    }
    return exitCode;
}
```

**核心不变式**:协程**只**通过 `coro::await*` 系列原语挂起,底层是 `boost::fibers::future::get()`。因此每次主 fiber 的 `yield()` 返回时,其余协程要么已阻塞在某个 await 上、要么已运行结束 —— 这保证随后的 `processEvents(WaitForMoreEvents)` 阻塞等待不会饿死任何就绪协程。

**显式让步**:`coro::yield()` 在让步前向 Qt 投递一个零延时唤醒事件(`QTimer::singleShot(0, ...)` 或自定义事件),确保 `processEvents` 不会因为还有"主动 yield 但未阻塞"的协程而误阻塞。

### 挂起 / 恢复机制

`await(obj, signal)` 的执行序列:
1. 创建 `boost::fibers::promise<Args>` 及其 `future`。
2. `QObject::connect(obj, signal, ...)` 一次性 lambda:`promise.set_value(args); disconnect();`。
3. 调用 `future.get()` → 挂起当前协程 fiber,让出调度器。
4. 信号在 `processEvents` 内触发(运行在主 fiber 上),`set_value` 使等待的协程 fiber 变为就绪。
5. `processEvents` 返回 → 驱动循环 `yield()` → 协程 fiber 恢复,`future.get()` 返回信号参数。

跨边界要点:`promise.set_value` 由主 fiber 调用(也是一个 fiber),对单线程默认调度器是安全操作。

### 驱动不变式修正(实现期发现)

最初设想"每次主 fiber 的 `yield()` 返回时,其余协程要么阻塞、要么结束"。但当一个协程因**另一个 fiber 完成**(而非 Qt 事件)而变就绪时(典型:`await(Task)` 等待的 `async` 协程跑完并 `set_value`),该唤醒发生在纯 fiber 调度阶段、不经过 `processEvents`。此时主 fiber 可能已(或即将)停在 `processEvents(WaitForMoreEvents)`,导致就绪协程被饿死、线程永久阻塞。

修正:**协程完成时主动唤醒 Qt 事件分发器**。`coro::async` 把协程体包一层,在体退出(正常返回或异常展开)时经 RAII `DriverWaker` 调用 `eventDispatcher()->wakeUp()`,产生一个粘性 pending-wake,使随后的 `processEvents` 立即返回 → 驱动循环再 `yield()` → 就绪协程恢复。该机制对调度器就绪队列的任意顺序都成立。信号/定时器/QFuture/QIODevice 的唤醒因来自 `processEvents` 内部,天然不需此处理。

### 已知限制

- **嵌套 Qt 事件循环**(如 `QDialog::exec()`、`QMessageBox::exec()`)期间,主 fiber 被卡在嵌套 `exec()` 内,协程 fiber 不会轮转。本期作为文档说明的已知限制,不在范围内解决。

## 3. 公开 API(分层,采用"风格 C")

命名空间 `coro`,库目标名 `qfcoro`。

### 3.1 底层(风格 A,日常顺序逻辑)

```cpp
namespace coro {
  int  exec();                              // 替代 app.exec()
  void quit(int code = 0);

  void launch(std::function<void()> fn);    // 在新 fiber 里跑,fire-and-forget
  void yield();                             // 协程间显式让步(含 Qt 唤醒投递)
  void sleep(std::chrono::milliseconds ms); // QTimer 实现,挂起 fiber 不阻塞线程

  // 通用信号 await,返回信号参数:0 参→void,1 参→该值,N 参→std::tuple
  template<class Obj, class Sig> auto await(Obj* obj, Sig sig);
}
```

### 3.2 上层(Task<T>,用于组合 / 跨边界)

```cpp
namespace coro {
  template<class T> class Task {            // 轻量句柄,内含 boost::fibers::future<T>
  public:
    T    get();                             // 等价 coro::await(task);异常重抛
    void then(std::function<void(T)> cb);   // 非阻塞:内部起 fiber 等结果后回调(可在 Qt 槽里用)
    bool done() const;
  };

  template<class Fn> auto async(Fn fn)
      -> Task<std::invoke_result_t<Fn>>;     // 启动协程并拿句柄
  template<class T>  T await(Task<T>& t);    // 等另一个 Task 完成取结果

  template<class... Ts> auto whenAll(Task<Ts>&...);  // 并发等全部,返回结果 tuple
  template<class... Ts> auto whenAny(Task<Ts>&...);  // 等第一个完成
}
```

`Task<void>` 特化:`get()` 返回 void;`then(std::function<void()>)`。

### 3.3 适配器(本期范围)

```cpp
namespace coro {
  template<class T> T    await(QFuture<T> f);  // QFutureWatcher 实现
                    void await(QFuture<void> f);
                    void await(QIODevice* dev); // 等 readyRead
  // sleep / await(signal) 见 3.1
}
```

**本期不做**:QNetworkReply、QProcess(机制相同,后续以同样模式扩展)。

## 4. 各适配器实现要点

- **signal**:`connect` 单次,回调内 `set_value` 后 `disconnect`;按信号参数个数萃取返回类型(0→void,1→值,N→tuple)。连接方式默认 `Qt::AutoConnection`(同线程即 Direct)。
  - **取前 K 个参数**(后续新增):`await<K>(obj, signal)`(`K` 为显式非类型模板参数)只返回信号前 K 个(decay 后)参数,模仿 Qt"槽可少于信号参数"的规则;`K=0`→void。内部仍连接信号完整参数 lambda,只转发前 K 个;`static_assert(K<=N)` 防止越界。与默认全参重载靠"显式非类型实参 vs 类型实参"消歧,无歧义、向后兼容。
- **timer / sleep**:`QTimer::singleShot(ms, &resume)` → 恢复 fiber。
- **QFuture<T>**:栈上 `QFutureWatcher<T>` watcher,`setFuture(f)`,connect `finished` → `set_value(watcher.result())`;若已 finished 则立即取值,避免错过信号。
- **QIODevice**:connect `readyRead`(并连接错误/关闭信号以便抛错或返回);单次唤醒。
- **错误处理**:风格 A 异常沿 fiber 栈正常传播;`Task<T>` 将异常存入 future,在 `get()` / `then` 时重抛;设备或 Future 出错抛 `coro::AwaitError`(继承 `std::runtime_error`)。
  - **实现状态(已延期)**:本版本**未实现** `coro::AwaitError`,`await(QIODevice*)` 也只连接 `readyRead`、未连接错误/关闭信号。设备/信号层的错误与取消统一归入"单次语义、无取消/超时"的已知限制(见 README 已知限制),留待后续版本。`Task<T>` 的异常传播已实现。

## 5. 线程模型与安全边界

- 单线程协作式:所有协程在主线程交替运行,协程之间**无数据竞争**(非抢占式,仅在 await 点切换)。
- `QtConcurrent` / `QFuture` 的计算在 Qt 线程池执行,本库只在主线程经 `QFutureWatcher` 取结果 —— 安全。
- **硬约束(文档明确)**:`coro::await*`、`coro::launch`/`async`、`coro::sleep` 只能在 `coro::exec()` 所在线程的 fiber 上下文中调用。

## 6. 工程结构

```
coro/
  CMakeLists.txt
  cmake/                 # 定位自定义 Boost 路径的辅助
  include/coro/
    coro.h               # 伞头文件
    core.h               # exec/quit/launch/yield/sleep
    task.h               # Task<T>、async、await(Task)
    signal.h             # await(obj, signal)
    timer.h              # sleep / await timer
    future.h             # await(QFuture)
    iodevice.h           # await(QIODevice)
    when.h               # whenAll / whenAny
    detail/
      promise.h          # fiber promise/future 封装、信号参数萃取
      driver.h           # 驱动循环、运行状态
  src/
    core.cpp
    driver.cpp
  examples/
    basic_signal.cpp     # launch + await(signal) + sleep
    future_demo.cpp      # await(QFuture) + whenAll
  tests/
    test_signal.cpp
    test_sleep.cpp
    test_future.cpp
    test_iodevice.cpp
    test_task_when.cpp
```

## 7. 构建

- CMake,C++17。
- 链接 `Qt5::Core`、`Boost::fiber`、`Boost::context`。
- 通过 `BOOST_ROOT` / `Boost_DIR` 指向 `/home/david/.aem/.../3rd-boost/9.0.0-alpha3-r1`(其 `lib/cmake/` 提供 config 文件);运行期需保证该路径在 `LD_LIBRARY_PATH` 或 rpath 中。

## 8. 测试策略

- 框架:**Qt Test**(环境已有 Qt;与 QTimer/事件循环天然集成),辅以 `QSignalSpy`。
- 用例通过 `coro::exec()` + 定时 `coro::quit()` 驱动,验证:
  - `await(signal)` 正确恢复并返回参数;
  - `sleep` 时长正确且不阻塞其他协程(并发性);
  - `await(QFuture)` 取到结果、异常重抛;
  - `await(QIODevice)` 在 readyRead 后恢复;
  - `Task`/`async`/`whenAll`/`whenAny` 组合语义;
  - 异常沿 fiber 栈与经 Task 的传播。
- 流程:**TDD —— 先写测试再实现**。

## 9. 范围与非目标(YAGNI)

- 本期范围:信号、定时器/sleep、QFuture、QIODevice、Task/async/when 组合、单线程泵驱动。
- 非目标(后续):QDBus、多线程/worker、自定义调度器、嵌套事件循环兼容。

### 后续已实现(同模式扩展)

- **QProcess**(`coro/process.h`,Qt5::Core,已并入伞头文件):`int await(QProcess*)` 等 `finished(int,ExitStatus)`、返回退出码。进程输出读取复用 `await(QIODevice*)`。
- **QNetworkReply**(`coro/network.h`,需 Qt5::Network,**opt-in** 不在伞头文件):`void await(QNetworkReply*)` 等 `finished`(出错也会触发,返回后由调用者 `readAll()`/查 `error()`),含 `isFinished()` 早退守卫。
