# 设计:`coro::qt_round_robin` —— 仿 `asio::round_robin` 的 Qt 事件循环 fiber 调度器

日期:2026-06-22

## 背景与动机

`awaitable::resolve()`(= `unbuffered_channel::push`)在**没有 consumer 停在 `await()`** 时会挂起
*调用它的上下文*。若该上下文是事件泵 fiber 且无其他就绪 fiber,boost.fiber 会去 `resume(nullptr)`
→ 段错误(见 `tests/test_resolve_crash.cpp`)。在自定义调度器的 `suspend_until()` 里 pump Qt 事件
同样崩溃:`suspend_until()` 跑在 **dispatcher 上下文**,而 dispatcher 绝不能被挂起;槽里
`resolve()` 的挂起会连带挂起 dispatcher → `context_initializer::active_` 置空 → crash
(见 `tests/test_sched_crash.cpp`)。

boost.fiber 官方 `examples/asio/round_robin.hpp` 给出了正确范式:**事件循环本身驱动 fiber 调度**,
事件由一个普通上下文(主 fiber)pump,`suspend_until()` 不 pump、不挂起。本设计把该范式适配到 Qt。

## 范围

- 新增独立头文件 `include/coro/qt_round_robin.h`,类 `coro::qt_round_robin`。
- **不改动** `coro::exec()` 与既有 `include/coro/qt_scheduler.h`。
- 新增测试 `tests/test_round_robin.cpp` 验证。
- 支持**跨线程**唤醒(`resolve()` 来自 `std::thread`),对齐 asio round_robin 语义。

## asio round_robin 机制(被模仿的对象)

```
service loop(跑在调用 io.run() 的主上下文里):
  while (! io.stopped()) {
    if (has_ready_fibers()) { while (io.poll()); cnd_.wait(lk); }   // 让 dispatcher 跑就绪 worker
    else                    { if (! io.run_one()) break; }          // 空闲:阻塞线程等一个 handler
  }
suspend_until(t): if (t!=max) 用 steady_timer 在 t 触发以唤醒 run_one;然后 cnd_.notify_one()
notify():         post 一个 handler 唤醒 run_one(可跨线程)
```
关键:`cnd_` 是 **fiber** condition_variable,协调 dispatcher 与 service loop(同线程);线程级阻塞在
`run_one()`(service loop 这个普通 fiber 里),因此槽/handler 在可挂起上下文运行 —— 安全。

## Qt 映射

| asio | Qt |
|------|----|
| `io.run_one()`(阻塞至 ≥1 handler) | `processEvents(WaitForMoreEvents \| AllEvents)` |
| `io.poll()`(非阻塞) | `processEvents(AllEvents)` |
| `boost::asio::post(io, fn)` / 唤醒 | `QAbstractEventDispatcher::wakeUp()`(线程安全) |
| `steady_timer`(suspend 定时) | `QTimer`(单次,调度器线程上) |
| `io.stopped()` | `running_` 标志 |

## 组件与状态

- `rqueue_`:fiber 就绪队列。
- `counter_`:**非 dispatcher** 就绪 fiber 计数(`has_ready_fibers()` 据此判断,忽略 dispatcher 上下文,
  与 asio 一致)。
- `mtx_` / `cnd_`:**fiber** mutex + condition_variable,协调 dispatcher 与 service loop(同线程)。
  不能用 `std::`,因为 `wait` 发生在 fiber 内。
- `suspend_timer_`:`QTimer` 成员,在调度器线程构造;`suspend_until(t)` 按 `t-now` 启动它,使
  `processEvents` 在 `t` 被一个 Qt 事件唤醒(覆盖使用 boost.fiber **原生**定时等待的 fiber,
  如 `this_fiber::sleep_for` / `future::wait_for`)。
- `disp_`:调度器线程的 `QAbstractEventDispatcher*`,构造时捕获;`notify()` 据此 `wakeUp()`(可跨线程)。
- `running_` 标志 + thread-local `instance()` 指针,供静态 `run()` / `stop()` 访问已安装的算法实例。

## 方法映射(asio → Qt)

- `awakened(ctx)`:`ctx->ready_link(rqueue_)`;非 dispatcher 上下文则 `++counter_`。
- `pick_next()`:弹出队首;非 dispatcher 则 `--counter_`。
- `has_ready_fibers()`:`counter_ > 0`。
- `suspend_until(t)`:若 `t != max`,在调度器线程上按 `t-now` 启动 `suspend_timer_`(此处就在调度器线程,
  操作 QTimer 安全);随后 `cnd_.notify_one()` 唤醒 service loop。**不阻塞、不 pump** —— 这是修复点。
- `notify()`:`disp_->wakeUp()`(线程安全),唤醒 service loop 阻塞中的 `processEvents`。用于跨线程唤醒。

## service loop(跑在主 fiber,经 `coro::qt_round_robin::run()`)

```cpp
while (running_) {
    if (has_ready_fibers()) {
        std::unique_lock<bf::mutex> lk(mtx_);
        cnd_.wait(lk);                              // 让 dispatcher 跑就绪 worker;
                                                    // suspend_until 在它们都挂起后唤醒我
    } else {
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents | QEventLoop::AllEvents);
        boost::this_fiber::yield();                 // 强制一次 dispatcher 轮转,
                                                    // 吸收 remote-ready / 到期 sleep 的 fiber
    }
}
```
`stop()`(由调度器线程上的 worker 调用):`running_=false; cnd_.notify_all(); disp_->wakeUp();`。

## 数据流(三类关键场景)

- **定时/sleep**:worker 挂起 → dispatcher 空闲 → `suspend_until` 启动 QTimer + notify cnd_ → service loop
  阻塞在 `processEvents` → QTimer 触发 → 返回 → `yield` → dispatcher 跑 worker。
- **信号 await(同线程)**:槽在 `processEvents` 内(普通 fiber 上下文)触发 → `resolve()` rendezvous 安全
  → worker 就绪 → 运行。**这正是诊断发现的崩溃类 —— 消失**,因为 pump 发生在 service-loop fiber,
  绝不在 dispatcher。
- **跨线程 `resolve()`**:远端 `push` 把 worker 排入本调度器的 remote-ready 队列并调用我们的 `notify()`
  → `wakeUp()` → `processEvents` 返回 → `yield` → dispatcher 排空 remote-ready → 运行 worker。

## 退出

service loop *就是*主 fiber,`stop()` 后正常退出循环;没有 detached service fiber 需要排空。worker fiber
应在 `run()` 返回前 join 或自然结束。`run()` 返回 `main`。

## 用法(测试/示例)

```cpp
QCoreApplication app(argc, argv);
bf::use_scheduling_algorithm<coro::qt_round_robin>();
bf::fiber([&]{ /* sleep/await 工作 */ coro::qt_round_robin::stop(); }).detach();
coro::qt_round_robin::run();   // 主 fiber 驱动 service loop 直到 stop()
```

## 测试(TDD,Qt Test)

`tests/test_round_robin.cpp`:
- (a) QTimer 唤醒推进一个 fiber(sleep 语义)。
- (b) 信号 `await` 在槽里 resolve 一个 park 着的 fiber —— 不崩。
- (c) `std::thread` 调用 `resolve()` 唤醒 park 着的 fiber(跨线程)。

加入 `tests/CMakeLists.txt`。

## 已知约束

- `stop()` 须由调度器线程上的 fiber 调用(用到 fiber cv `notify`)。跨线程终止需另设 atomic + wakeUp,
  本设计暂不覆盖。
- `suspend_timer_` 复用单个 QTimer;构造/析构均在调度器线程。
