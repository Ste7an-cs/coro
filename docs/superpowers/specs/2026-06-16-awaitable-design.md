# 设计文档:awaitable<T> 异步数据同步原语

- 日期:2026-06-16
- 状态:已确认设计,待 review 后进入实现计划
- 关联:[2026-06-12 qfcoro 设计](2026-06-12-qt-fiber-coroutine-design.md)

## 1. 背景与目标

现有各适配器(signal/future/iodevice/process/network)均以一次性的
`boost::fibers::promise<T>` + `future<T>` 在「事件回调」与「等待中的协程」之间传递数据。
promise/future 是**单次**语义:一次 `set_value` 对一次 `get`。

本设计引入一个可复用的 `awaitable<T>` 抽象,作为异步数据同步的统一原语:

- `await()` —— 协程侧等待数据;
- `resolve(v)` —— 生产侧(事件回调)提交数据;
- `close()` —— 关闭 awaitable,使当前/后续的 `await()` 得到「已关闭」结果。

内部使用 **`boost::fibers::unbuffered_channel<T>`**(rendezvous 语义)。
首个落地目标:把**信号适配器** `coro::await(obj, signal)` 的底层从 promise/future
改为 `awaitable<T>` 传递数据。

## 2. 语义决策(已确认)

| 决策点 | 选择 | 说明 |
|---|---|---|
| 生命周期 | **可复用(多次)** | 一个 awaitable 可被多次 `resolve` / 多次 `await`,形成数据流,直到 `close`。 |
| `await()` 返回 | **`result<T>`** | 显式区分「取到值」与「已关闭」。 |
| 底层通道 | **`unbuffered_channel<T>`** | rendezvous:`push` 挂起生产者直到被消费。 |
| 无 await 时 resolve | **阻塞直到有人 await**(rendezvous 契约) | 见 §4 分析:信号用例下不会发生(await 总先就绪)。 |
| `result<T>` 形态 | **value \| closed**(无 exception) | 信号无错误通道,YAGNI;未来需要再加。 |
| `coro::await(signal)` 对外返回 | **保持解包后的 `T`**(非 `result<T>`) | 现有 API/测试不变;closed 时抛异常,沿用今日 `broken_promise` 行为。 |

## 3. 接口设计

### 3.1 `result<T>`

值-或-关闭的轻量结果类型,不携带异常。

```cpp
namespace coro {

template<class T>
class result {
public:
    static result value(T v);          // 构造「有值」
    static result closed_();           // 构造「已关闭」

    bool has_value() const noexcept;   // true = 取到值
    bool closed()    const noexcept;   // true = 通道关闭且无值
    explicit operator bool() const noexcept { return has_value(); }

    T&  value() &;                     // 仅 has_value() 时有效
    T&& value() &&;
private:
    std::optional<T> v_;               // nullopt ⇒ closed
};

// void 特化:仅携带 closed/has_value 状态,无 payload
template<>
class result<void> {
public:
    static result value();
    static result closed_();
    bool has_value() const noexcept;
    bool closed()    const noexcept;
    explicit operator bool() const noexcept { return has_value(); }
};

} // namespace coro
```

### 3.2 `awaitable<T>`

```cpp
namespace coro {

template<class T>
class awaitable {
public:
    awaitable() = default;
    awaitable(const awaitable&) = delete;            // 不可拷贝
    awaitable& operator=(const awaitable&) = delete;

    // 协程侧:等待一个值。
    // - 通道开启:阻塞当前 fiber 直到 resolve 提交一个值 ⇒ result::value
    // - 通道已关闭:立即返回 result::closed_()
    result<T> await();

    // 生产侧:提交一个值(rendezvous,push 直到被消费)。
    // 返回 false 表示通道已关闭(提交失败)。
    bool resolve(T v);

    // 关闭通道:正在 await 的协程与后续 await 均得到 closed 结果;
    // 后续 resolve 返回 false。可重复调用(幂等)。
    void close();

private:
    boost::fibers::unbuffered_channel<T> ch_;
};

// void 特化:内部用 unbuffered_channel<std::monostate>
template<>
class awaitable<void> {
public:
    result<void> await();
    bool         resolve();
    void         close();
private:
    boost::fibers::unbuffered_channel<std::monostate> ch_;
};

} // namespace coro
```

实现要点:

- `await()`:`channel_op_status st = ch_.pop(v);`
  - `success` ⇒ `result<T>::value(std::move(v))`
  - `closed`  ⇒ `result<T>::closed_()`
- `resolve(v)`:`return ch_.push(std::move(v)) == channel_op_status::success;`
  - 通道关闭时 `push` 返回 `closed` ⇒ `resolve` 返回 `false`。
- `close()`:`ch_.close();`(boost 通道 `close` 幂等)。
- 适配器中以 `std::make_shared<awaitable<T>>()` 持有,生命周期由 `await` 协程与
  连接 lambda 共享(对齐现有 promise 的 shared_ptr 用法)。

## 4. 单线程泵模型下 unbuffered 的正确性分析

**不变式**:在泵-驱动模型里,Qt 槽 lambda 只能在驱动 fiber 的 `processEvents`
内运行;而协程 fiber 一旦让步(进入 `pop()` 挂起),控制权才回到驱动 fiber。

- 信号适配器流程:`connect(...)` → `await()`(`pop` 挂起)→ 让步回驱动 → `processEvents`。
  因此**信号能够触发时,消费者必定已挂起在 `pop()`**。槽内 `resolve()`→`push()`
  立即与等待中的 `pop()` 完成 rendezvous 并返回,**不会死锁**。
- `connect` 与 `pop` 之间不会被事件打断(单线程,槽要等协程让步才运行),故不存在
  「信号在 await 之前到达而错过」的新窗口(与今日 promise 版本一致)。
- 多次复用:握手期间消费者会在 `push` 返回前**同步地重新武装** `pop()`,故下一次
  emission 同样能找到等待者。
- 唯一会阻塞的情况:**没有任何 await 等待、且未来也不会有时调用 `resolve()`** ——
  这是 unbuffered 的固有契约。信号用例不触发此情况;通用使用需调用方保证有等待者。

**生产者必须事件循环驱动(实现期发现)**:`resolve()`/`close()` 必须从 Qt 事件回调
(`QTimer`、信号槽)调用。若改由另一个忙等协程作生产者(`launch` 里 `for` 循环 resolve),
握手后驱动 fiber 可能在仍有就绪消费者协程时进入 `processEvents(WaitForMoreEvents)`
阻塞,饿死该消费者 → 死锁。awaitable 的目标用例(信号槽内 resolve)天然满足此约束。

## 5. 信号适配器迁移

`include/coro/signal.h` 中 `await_signal_impl` / `await_typed_impl` 的改动:

- 将 `promise<R>` / `future<R>` 替换为 `std::make_shared<awaitable<R>>()`。
- 连接 lambda 中:`if (void) aw->resolve(); else aw->resolve(PR::make(a...));`,
  随后 `disconnect`(单次触发,语义不变)。
- **关闭守卫(保留今日 broken_promise 语义)**:由于 awaitable 由 lambda 与等待协程
  共享 shared_ptr,仅销毁 lambda 不会关闭通道。lambda 额外捕获一个 RAII 守卫,其析构
  调用 `aw->close()`。于是:对象被销毁 / 连接被移除 ⇒ lambda 连同守卫析构 ⇒ 通道关闭
  ⇒ 等待中的 `await()` 立即得到 `closed()` ⇒ 抛异常。这复刻了今日「promise 被销毁
  ⇒ `future.get()` 抛 broken_promise」的行为(否则协程会永久挂起)。
- 等待:`auto r = aw->await();`
  - `r.has_value()` ⇒ 返回 `std::move(r).value()`(void 则直接返回)。
  - `r.closed()`    ⇒ 抛异常(见 §7)。
- N 参数打包逻辑(`pack_result`、`set_typed`、`await<Types...>`)保持不变,
  仅数据传输载体从 promise 改为 awaitable。

> 本次**仅**迁移信号适配器;future/iodevice/process/network 暂不改动,留待后续。

## 6. 文件与构建

- 新增 `include/coro/awaitable.h`(`result<T>` + `awaitable<T>` 及其 void 特化,
  以及异常类型 `coro::awaitable_closed : std::runtime_error`)。
- 在伞头文件 `include/coro/coro.h` 中聚合 `awaitable.h`。
- `include/coro/signal.h` 改为依赖 `awaitable.h`。
- 头文件库,无需改 `CMakeLists.txt` 的链接项。

## 7. 错误处理

- `result<T>` 不携带异常;关闭即「无值」。
- `coro::await(signal)` 对外:closed ⇒ 抛异常。今日靠 `future.get()` 抛
  `boost::fibers::broken_promise`;迁移后无 promise,故抛一个 coro 专属异常
  `coro::awaitable_closed`(继承 `std::runtime_error`)。`tests/test_signal.cpp`
  覆盖的是正常触发路径,异常类型变化不影响其通过。
- `resolve` 在通道关闭后返回 `false`,生产者据此感知关闭,不抛异常。

## 8. 测试计划

新增 `tests/test_awaitable.cpp`,覆盖:

1. **基本同步**:协程 `await()`,另一协程 `resolve(v)` ⇒ 取到值。
2. **多次复用**:多次 `resolve`/`await` 交替,顺序正确。
3. **void 特化**:`awaitable<void>` 的 `resolve()`/`await()`。
4. **关闭语义**:`close()` 后,正在 `await` 的协程得到 `closed()`;后续 `await` 亦 `closed()`;
   后续 `resolve` 返回 `false`。
5. **rendezvous**:`resolve` 在 `await` 就绪前后均能正确握手(单线程泵下的时序)。

回归:现有 `tests/test_signal.cpp` 在迁移后须全部通过(对外行为不变)。

## 9. 非目标(YAGNI)

- `result<T>` 不携带 `exception_ptr`/错误码。
- 不迁移 future/iodevice/process/network 适配器。
- 不引入取消/超时(沿用库现状的「单次语义、无取消/超时」限制)。
- 不改变 `coro::await(signal)` 的对外签名。
