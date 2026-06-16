# Changelog

本项目所有重要变更记录于此文件。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/),
版本遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

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
