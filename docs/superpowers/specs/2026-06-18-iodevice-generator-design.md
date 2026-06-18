# QIODevice 字节块生成器(`coro::generate`)设计

日期:2026-06-18
状态:已批准,待实现

## 背景与目标

现有 `coro::await(QIODevice*)`(`include/coro/iodevice.h`)只等待**一次** `readyRead`,返回后
由调用者自行读取数据。对于 QTcpSocket 等流式 IO,使用者通常需要"持续消费到对端关闭"的循环
读取模式。本设计为 QIODevice(因此自动覆盖 QTcpSocket —— 它本身就是 QIODevice)新增一个
**字节块生成器**:每拉取一次,挂起当前协程直到下一次 `readyRead`,产出 `dev->readAll()` 的
`QByteArray`;流结束时正常终止。

### 范围(明确边界)

- **不改动** `await(QIODevice*)` 及其他任何适配器,保持现状。
- **不新增** QTcpSocket 专用的连接类 await(connected / disconnected / 写入排空)。
- **不新增** 函数化便捷读取(read N 字节 / readLine)。
- 本次唯一交付物:`include/coro/iodevice.h` 中的字节块生成器,并入伞头文件。

## API

```cpp
namespace coro {

class io_byte_generator {
public:
    io_byte_generator(io_byte_generator&&) noexcept;   // move-only
    io_byte_generator(const io_byte_generator&) = delete;

    // 显式拉取:挂起直到下一块;流结束 → result.closed()。
    result<QByteArray> next();

    // range-for 支持(基于 next() 实现)。
    class iterator;
    iterator begin();   // 拉取首块
    iterator end();     // closed 哨兵
};

// 工厂函数。
io_byte_generator generate(QIODevice* dev);

} // namespace coro
```

两种消费方式均支持:

```cpp
// 方式 A:range-for
for (QByteArray chunk : coro::generate(&sock)) {
    process(chunk);
}

// 方式 B:显式 next()
auto gen = coro::generate(&sock);
while (auto r = gen.next()) {
    QByteArray b = std::move(r).value();
    process(b);
}
```

`begin()` 拉取首块并缓存;`iterator::operator++` 调用 `next()`;迭代器与 `end()` 的比较即
"当前结果是否 closed"。迭代自然结束 = 流关闭。

## `next()` 语义

设备以 `QPointer<QIODevice>` 持有,以便安全检测销毁。每次 `next()`:

1. 若设备已销毁(`QPointer` 为空)→ 返回 `result<QByteArray>::closed_()`。
2. 若 `dev->bytesAvailable() > 0` → 返回 `result::value(dev->readAll())`。
   先排空已缓冲数据:单线程泵-驱动模型下,`bytesAvailable` 检查与后续挂起之间不会让出
   协程,因此不存在"边沿丢失"竞态。
3. 若已标记流结束(EOS)→ 返回 `closed_()`。
4. 否则挂起,等待以下任一信号(单次连接,唤醒后立即断连):
   - `readyRead` → 回到步骤 2(读取本次到达的数据)。
   - `readChannelFinished` → 置 EOS 标记,回到步骤 2(排空尾部残留字节,随后下次返回 closed)。
   - `QObject::destroyed` → 返回 `closed_()`。

   连接采用**每次拉取建立、唤醒即断开**的单次模式,与现有 `await(QIODevice*)` 一致,避免
   rendezvous 槽长期占用。底层用一个单次 `awaitable` 在以上信号间汇合。

## 错误与边界处理

- **流结束是正常终止**:对端断开 / EOF / 设备销毁 → `closed()`,**绝不抛异常**(与
  `await(signal)` 不同 —— 迭代至完成是预期行为,closed 即正常结束,不视作异常)。
- **绝不产出空块**:由 `bytesAvailable > 0` 守卫,跳过空 `readAll()`。
- **上下文约束**:与所有适配器一致,必须在 `coro::exec()` 所在线程的协程上下文中调用。
- **单消费者、非线程安全**:符合本库单线程模型。
- **move-only**:生成器持有连接与设备指针,禁止拷贝。

## 文件改动

- `include/coro/iodevice.h` —— 新增 `io_byte_generator` 与 `generate()`;保留现有
  `await(QIODevice*)`。需 `#include` `coro/awaitable.h`、`<QPointer>`、`<QByteArray>`。
- `include/coro/coro.h` —— 确认 `iodevice.h` 已在伞头文件中(现已包含,无需改动)。
- `tests/test_iodevice.cpp` —— 新增测试(见下),无需新增依赖。
- `tests/CMakeLists.txt` —— `test_iodevice` 已构建,无需改动。
- `CHANGELOG.md` —— 记录新增;随后推送 origin(项目工作流)。
- `README.md` —— API 章节补充 `coro::generate(QIODevice*)`。

## 测试方案(TDD)

本库测试不引入网络依赖,故用一个**顺序 `QIODevice` 桩**(`SeqDevice : public QIODevice`,
`isSequential() == true`)模拟流式设备:测试可向其内部缓冲投递数据并发 `readyRead`,在流末
显式 `emit readChannelFinished()` 以触发生成器正常结束。

1. **`next()` 逐块**:用 `QTimer` 在拉取间隔向桩设备投递若干块并发 `readyRead`,断言每次
   `next()` 取得对应块;末尾 `emit readChannelFinished()` 后 `next()` 返回 `closed()`。
2. **range-for 收集**:对生成器做 range-for,把所有块拼接,与投递总数据比对;`readChannelFinished`
   触发迭代自然结束。
3. **设备销毁**:在等待期间销毁桩设备,断言生成器以 `closed()` 正常终止、不抛异常。

> 说明:`QBuffer::close()` 不发 `readChannelFinished`,故采用上述顺序桩设备以确定性地覆盖
> "流结束"(`readChannelFinished`)与"设备销毁"两条终止路径。

## 已知限制(沿用本库现状)

- 无取消 / 超时:若对端既不发数据也不关闭,生成器会一直挂起。
- 退出前应让消费协程跑完(参见 README"退出前请确保协程完成")。
