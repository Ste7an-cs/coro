# Changelog

本项目所有重要变更记录于此文件。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/),
版本遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added — `coro::qt_round_robin` Qt 事件循环 fiber 调度器

- **`coro::qt_round_robin`**(`coro/qt_round_robin.h`,已并入伞头文件 `coro/coro.h`):仿 boost.fiber
  官方 `examples/asio/round_robin.hpp` 的 Qt 调度器。**事件循环本身驱动 fiber 调度** —— 主 fiber 经
  `qt_round_robin::run()` 跑 service loop,Qt 事件在该 service-loop fiber(可挂起的普通上下文)里 pump;
  `suspend_until()` 跑在 dispatcher 上下文,**绝不 pump、绝不挂起**,只为 boost.fiber 原生定时等待
  (`this_fiber::sleep_for` 等)启动一个单次 `QTimer` 唤醒并 `notify` service loop。`notify()`(可跨线程)
  经按线程查询到的 `QAbstractEventDispatcher::wakeUp()` 唤醒阻塞中的 `processEvents`,以排空 remote-ready /
  到期 sleep 的 fiber。这样槽里的 `resolve()`(= `unbuffered_channel::push` 的潜在挂起)发生在 service-loop
  fiber 而非 dispatcher,**根除**了下述诊断出的崩溃类。`run()`/`stop()` 为静态接口(`stop()` 须由调度器线程
  上的 fiber 调用)。
- 实现要点(三个易踩的坑):
  - suspend QTimer 须用 **`Qt::PreciseTimer`**:默认 `Qt::CoarseTimer` 可提前至多 5% 触发,早于 fiber 真实
    `steady_clock` 唤醒时刻 → `sleep2ready` 判未到期不唤醒、单次 QTimer 已耗尽 → 永久挂死(间歇性 hang 根因)。
  - QTimer 间隔**向上取整**到毫秒 —— 截断(floor)同样会让 QTimer 早于 deadline 触发,后果同上。
  - `notify()`/`stop()` 按 `QThread*` **重新查询** dispatcher 而非缓存指针 —— 退出期 `QCoreApplication` 先析构,
    缓存指针会悬空导致收尾时 use-after-free。
- 新增测试 `test_round_robin`:(a) `QTimer` 唤醒推进原生 `sleep_for` 挂起的 fiber;(b) 槽里 `resolve` 一个
  park 着的 fiber 不崩;(c) `std::thread` 跨线程 `resolve` 唤醒 park 着的 fiber。
- 设计文档:`docs/superpowers/specs/2026-06-22-qt-round-robin-design.md`。

### Added — 崩溃诊断:dispatcher 上下文不可挂起

- 诊断并固化两类崩溃复现(均源于在 **dispatcher 上下文**或**无 consumer 的事件泵 fiber**上触发
  `awaitable::resolve()` → `unbuffered_channel::push` 挂起):
  - `tests/test_resolve_crash.cpp`:在事件循环线程对「无 fiber 停在 `await()`」的 awaitable 调用 `resolve()`
    会挂起事件泵 fiber、无可运行 fiber → `resume(nullptr)` 崩溃(`control`/`double`/`no_awaiter`/`before_await`/
    `stdthread*` 等场景;无参跑 `control` 作回归)。
  - `tests/test_sched_crash.cpp`:自定义调度器在 `suspend_until()`(dispatcher 上下文)里 `processEvents`,槽里
    `resolve()` 挂起连带挂起 dispatcher → `context_initializer::active_` 置空 → 段错误;对照 `good`/`safe` 模式
    由普通 fiber 泵事件则正常。
- 配套**安全调度器** `coro::qt_scheduler`(`coro/qt_scheduler.h`):`suspend_until()` 只在条件变量上阻塞等待,
  不 pump、不挂起任何 fiber;事件交由普通 fiber 泵,使槽里的 `resolve()` 在可挂起上下文运行。
  (后续 `coro::qt_round_robin` 给出对齐 asio 的更完整范式。)

### Added — QIODevice 字节块生成器

- **`coro::generate(QIODevice*)`**(`coro/iodevice.h`,已在伞头文件中):返回 move-only 的
  `coro::io_byte_generator`。`next()` 挂起当前协程直到下一次 `readyRead`,产出 `dev->readAll()`
  的 `QByteArray`(经 `result<QByteArray>` 传递,值/关闭);每次拉取先排空 `bytesAvailable()`,
  单线程模型下无边沿丢失。`readChannelFinished` 或设备销毁时**正常**结束(`closed()`,不抛)。
  支持 range-for 与显式 `next()` 两种消费方式。底层在单次 `awaitable<int>` 上汇合
  `readyRead`/`readChannelFinished`/`destroyed`,槽内先断开全部连接以避免 rendezvous 二次 push 挂起。
- 新增测试 `test_iodevice`:`nextYieldsChunks`(逐块)、`destroyEndsClean`(销毁正常终止)、
  `rangeForCollects`(range-for 收集);采用顺序 `QIODevice` 桩,无网络依赖。
- 新增 opt-in 测试 `test_iodevice_socket`(`generateOverTcpSocket`):`QTcpServer`/`QTcpSocket`
  回环验证生成器在真实 socket 上的逐块产出与断开(`readChannelFinished`)正常终止;链接
  `Qt5::Network`,与 `test_iodevice` 隔离以保持后者无网络依赖。
- 现有 `await(QIODevice*)` 保持不变。

### Added — awaitable<T> 异步数据同步原语

- **`coro::awaitable<T>`**(`coro/awaitable.h`,已并入伞头文件):基于
  `boost::fibers::unbuffered_channel` 的**可复用** rendezvous 原语。`await()` 取一个值返回
  `coro::result<T>`(值-或-关闭);`resolve(v)` 提交数据(rendezvous,关闭后返回 `false`);
  `close()` 关闭通道并唤醒等待者。含 `awaitable<void>` 特化与异常类型 `coro::awaitable_closed`。
- **信号 await 迁移**:`coro::await(obj, signal)` / `coro::await<Types...>(...)` 底层由
  `promise/future` 改为 `awaitable<T>` 传递数据;对外返回类型与行为不变。连接 lambda 持有
  关闭守卫(`connect` 后 `guard.reset()` 使 lambda 独占),对象销毁/断连时关闭 awaitable,
  使等待中的 `await` 抛 `awaitable_closed`(复刻旧版 `broken_promise`)。
- 新增测试 `test_awaitable`(基本同步/多次复用/void 特化/close 语义);`test_signal`
  新增「对象销毁 → 抛 `awaitable_closed`」回归。全量 10 个测试套件通过。
- 注:`resolve()`/`close()` 须由事件循环驱动(信号槽/`QTimer`),单线程泵下不能用忙等协程作生产者。

### Added — 信号 await 按 Qt 槽形参类型取参数

- **`coro::await<Types...>(obj, &T::signal)`**:像 Qt 槽那样**指定形参类型**,所指定的类型即
  `await` 的返回类型(`1`→该值,`N`→`std::tuple<Types...>`);默认 `await(obj, signal)`(取全部)
  保持不变、向后兼容。`Types...` 须对应信号前若干个参数,且可与之做类型转换(如信号 `int` → 取 `double`)。
  与默认重载靠"显式类型实参 vs 无实参"消歧,无歧义;指定个数超过信号参数个数触发 `static_assert`。
- `test_signal` 新增用例:`await<int>`、`await<int,QString>`、`await<int,QString,double>`、
  以及类型转换 `await<double>`(信号发 `int`)。
- (取代了早先按个数的 `await<K>` 设计。)

### Added — 网络/进程适配器

- **`coro::await(QProcess*)`**(`coro/process.h`,已并入伞头文件 `coro/coro.h`):等待进程
  `finished(int, ExitStatus)`,返回退出码。进程输出读取复用 `await(QIODevice*)`。
- **`coro::await(QNetworkReply*)`**(`coro/network.h`,**opt-in**,需链接 `Qt5::Network`,
  不在伞头文件中):等待 `finished`(出错时也会触发),返回后由调用者 `readAll()` / 查 `error()`;
  含 `isFinished()` 早退守卫。
- 新增测试 `test_process`、`test_network`(后者用 `file://` URL 离线确定性验证);全量 9 个测试套件通过。

### Added — 0.1.0 首个实现(基于 Boost.Fiber 的 Qt 协程库)

- **核心驱动**:单线程"泵驱动"模型。`coro::exec()` 取代 `app.exec()`,交替
  `boost::this_fiber::yield()` 与 `QCoreApplication::processEvents(WaitForMoreEvents)`;
  `coro::quit()` 退出主循环。`coro::launch()` 启动协程,`coro::yield()` 显式让步。
- **`coro::sleep(ms)`**:基于 `QTimer` 挂起当前 fiber,不阻塞事件循环(并发不互相阻塞)。
- **`coro::await(QObject*, &T::signal)`**:通用信号 await,按参数个数返回 `void` / 值 / `std::tuple`。
- **`Task<T>` / `coro::async` / `coro::await(Task)`**:协程句柄、启动与等待;`Task::then()`
  非阻塞续接、`get/wait/done`;异常沿 fiber 栈与经 future 传播。
  - 修复泵驱动模型下的死锁:`async` 协程完成时经 RAII `DriverWaker` 唤醒 Qt 事件分发器,
    避免等待结果的协程被饿死。
- **`coro::whenAll(...)`**:并发等待全部 Task,返回结果 `tuple`(各结果类型需非 void)。
- **`coro::whenAny(...)`**:返回首个完成的 Task 下标(1ms 轮询、非消费式 `done()`)。
- **`coro::await(QFuture<T>)` / `await(QFuture<void>)`**:经 `QFutureWatcher` 等待 QtConcurrent 结果。
- **`coro::await(QIODevice*)`**:等待 `readyRead`(单次)。
- **工程**:CMake 构建(Qt5 Core/Test/Concurrent + Boost.Fiber/Context 1.74),
  7 个 Qt Test 测试套件全部通过,伞头文件 `coro/coro.h`,示例 `basic_signal` / `future_demo`,
  README 与设计/实现文档。

### 已知限制

- 嵌套 Qt 事件循环(如 `QDialog::exec()`)期间协程不轮转。
- `await(signal)` / `await(QIODevice*)` 为单次语义,无取消/超时;对象在等待期间销毁或信号
  永不触发会导致协程永久挂起(可能抛 `broken_promise`)。
- `await(Task)` 为单次消费;`whenAll` 要求结果非 void。
- 退出前应确保协程跑完,否则挂起的 detached fiber 可能在进程退出时空转。
- `coro::AwaitError` 与设备错误信号处理延期至后续版本。

[Unreleased]: https://github.com/Ste7an-cs/coro/commits/master
