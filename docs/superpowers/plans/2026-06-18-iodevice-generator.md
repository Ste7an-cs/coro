# QIODevice 字节块生成器(`coro::generate`)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 QIODevice(因此自动覆盖 QTcpSocket)新增一个字节块生成器 `coro::generate(QIODevice*)`,每拉取一次挂起协程直到下一次 `readyRead`,产出 `readAll()` 块,流结束时正常终止。

**Architecture:** 在 `include/coro/iodevice.h` 中新增 move-only 类 `io_byte_generator` 及工厂 `generate()`,复用现有 `coro::awaitable<T>`/`result<T>` 原语。`next()` 每次先排空 `bytesAvailable()`(单线程模型无边沿丢失竞态),否则在一个单次 `awaitable<int>` 上汇合 `readyRead`/`readChannelFinished`/`destroyed` 三个信号。range-for 由输入迭代器封装 `next()`。

**Tech Stack:** C++17、Qt5(Core/Test)、Boost.Fiber 1.74、单线程泵-驱动模型(`coro::exec()`)。

## Global Constraints

- C++17;遵循现有 `include/coro/` 头文件风格(头文件内联实现)。
- 必须在 `coro::exec()` 所在线程的协程上下文(`coro::launch`/`async`)中调用所有 `await`/`next`。
- 单线程、单消费者、非线程安全。
- `iodevice.h` 在伞头文件 `coro/coro.h` 中,本特性无需 opt-in、不引入网络依赖。
- **绝不**改动现有 `await(QIODevice*)` 及其他适配器。
- 流结束(`readChannelFinished` / 设备销毁)是**正常终止** → `closed()`,**绝不抛异常**。
- 运行测试需 `LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib`。
- 完成后更新 `CHANGELOG.md` 并推送到 GitHub origin(项目工作流)。

---

### Task 1: 生成器核心 `io_byte_generator::next()` + 顺序设备桩

**Files:**
- Modify: `include/coro/iodevice.h`(在现有 `await(QIODevice*)` 之后追加)
- Test: `tests/test_iodevice.cpp`(新增 `SeqDevice` 桩与两个测试槽)

**Interfaces:**
- Consumes: `coro::result<T>`、`coro::awaitable<T>`(来自 `coro/awaitable.h`)。
- Produces:
  - `class coro::io_byte_generator`(move-only),含 `coro::result<QByteArray> next();`
  - `coro::io_byte_generator coro::generate(QIODevice* dev);`
  - Task 2 将向同类追加 `iterator` / `begin()` / `end()`。

- [ ] **Step 1: 写失败测试(逐块拉取 + 设备销毁正常终止)**

在 `tests/test_iodevice.cpp` 顶部 include 区追加(若尚无):
```cpp
#include <QByteArray>
#include <cstring>
```

在 `class TestIoDevice` **之前**新增顺序设备桩:
```cpp
// 顺序 QIODevice 桩：feed() 投递数据并发 readyRead；finish() 发 readChannelFinished。
class SeqDevice : public QIODevice {
    Q_OBJECT
public:
    explicit SeqDevice(QObject* p = nullptr) : QIODevice(p) { open(QIODevice::ReadOnly); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return buf_.size() + QIODevice::bytesAvailable(); }
    void feed(const QByteArray& d) { buf_.append(d); emit readyRead(); }
    void finish() { emit readChannelFinished(); }
protected:
    qint64 readData(char* data, qint64 max) override {
        qint64 n = qMin<qint64>(max, qint64(buf_.size()));
        std::memcpy(data, buf_.constData(), size_t(n));
        buf_.remove(0, int(n));
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray buf_;
};
```

在 `TestIoDevice` 的 `private slots:` 内追加:
```cpp
    void nextYieldsChunks() {
        SeqDevice dev;
        QList<QByteArray> got;
        coro::launch([&]{
            auto gen = coro::generate(&dev);
            while (auto r = gen.next()) got.append(std::move(r).value());
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev.feed("aa"); });
        QTimer::singleShot(20, [&]{ dev.feed("bbb"); });
        QTimer::singleShot(30, [&]{ dev.finish(); });
        coro::exec();
        QCOMPARE(got.size(), 2);
        QCOMPARE(got[0], QByteArray("aa"));
        QCOMPARE(got[1], QByteArray("bbb"));
    }

    void destroyEndsClean() {
        auto* dev = new SeqDevice;
        bool ended = false, threw = false;
        coro::launch([&]{
            auto gen = coro::generate(dev);
            try { while (auto r = gen.next()) { (void)r; } ended = true; }
            catch (...) { threw = true; }
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev->feed("x"); });
        QTimer::singleShot(20, [&]{ delete dev; });
        coro::exec();
        QVERIFY(ended);
        QVERIFY(!threw);
    }
```

- [ ] **Step 2: 运行测试,确认编译失败**

Run: `cmake --build build -j --target test_iodevice`
Expected: 编译失败 —— `'generate' is not a member of 'coro'` / `io_byte_generator` 未声明。

- [ ] **Step 3: 实现 `io_byte_generator::next()` 与 `generate()`**

在 `include/coro/iodevice.h` 顶部补充 include:
```cpp
#include <array>
#include <QByteArray>
#include <QPointer>
#include "coro/awaitable.h"
```

在现有 `await(QIODevice*)` 之后、`} // namespace coro` 之前追加:
```cpp
// 字节块生成器：每拉取一次挂起直到下一次 readyRead，产出 dev->readAll()。
// 流结束（readChannelFinished / 设备销毁）→ next() 返回 closed()，绝不抛异常。
class io_byte_generator {
public:
    explicit io_byte_generator(QIODevice* dev) : dev_(dev) {}
    io_byte_generator(io_byte_generator&&) noexcept = default;
    io_byte_generator& operator=(io_byte_generator&&) noexcept = default;
    io_byte_generator(const io_byte_generator&) = delete;
    io_byte_generator& operator=(const io_byte_generator&) = delete;

    // 显式拉取：挂起直到下一块；流结束 → result.closed()。
    result<QByteArray> next() {
        for (;;) {
            if (!dev_) return result<QByteArray>::closed_();           // 已销毁
            if (dev_->bytesAvailable() > 0)
                return result<QByteArray>::value(dev_->readAll());    // 先排空缓冲
            if (eos_) return result<QByteArray>::closed_();           // 结束且无残留

            // 在单次 awaitable 上汇合 readyRead / readChannelFinished / destroyed。
            // 槽内先断开全部连接，避免第二个槽对已被消费的 rendezvous 通道再次 push 而挂起。
            awaitable<int> aw;   // 1 = 可读，0 = 流结束
            auto conns = std::make_shared<std::array<QMetaObject::Connection, 3>>();
            auto disconnectAll = [conns]{ for (auto& c : *conns) QObject::disconnect(c); };

            (*conns)[0] = QObject::connect(dev_, &QIODevice::readyRead,
                [&aw, disconnectAll]{ disconnectAll(); aw.resolve(1); });
            (*conns)[1] = QObject::connect(dev_, &QIODevice::readChannelFinished,
                [&aw, disconnectAll]{ disconnectAll(); aw.resolve(0); });
            (*conns)[2] = QObject::connect(dev_, &QObject::destroyed,
                [&aw, disconnectAll]{ disconnectAll(); aw.close(); });

            auto r = aw.await();
            disconnectAll();   // 幂等：正常唤醒时槽已断开；销毁时连接已失效

            if (r.closed()) return result<QByteArray>::closed_();     // destroyed
            if (r.value() == 0) eos_ = true;                          // 结束：回环排空残留
            // 否则回环 → readAll()
        }
    }

private:
    QPointer<QIODevice> dev_;
    bool eos_ = false;
};

inline io_byte_generator generate(QIODevice* dev) {
    return io_byte_generator(dev);
}
```

- [ ] **Step 4: 运行两个测试,确认通过**

Run:
```
cmake --build build -j --target test_iodevice && \
LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib \
./build/tests/test_iodevice nextYieldsChunks destroyEndsClean
```
Expected: `Totals: 2 passed, 0 failed`。

- [ ] **Step 5: 运行整个 test_iodevice,确认未回归(含原 awaitReadyRead)**

Run:
```
LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib \
./build/tests/test_iodevice
```
Expected: `Totals: 3 passed, 0 failed`。

- [ ] **Step 6: 提交**

```bash
git add include/coro/iodevice.h tests/test_iodevice.cpp
git commit -m "feat: io_byte_generator::next() 字节块生成器(QIODevice)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: range-for 迭代器支持

**Files:**
- Modify: `include/coro/iodevice.h`(`io_byte_generator` 类内追加 `iterator`/`begin`/`end`)
- Test: `tests/test_iodevice.cpp`(新增 range-for 测试槽)

**Interfaces:**
- Consumes: `io_byte_generator::next()`(Task 1)。
- Produces:
  - `class io_byte_generator::iterator`(输入迭代器,`operator*`/`operator++`/`operator!=`)
  - `iterator io_byte_generator::begin();`、`iterator io_byte_generator::end();`

- [ ] **Step 1: 写失败测试(range-for 收集)**

在 `TestIoDevice` 的 `private slots:` 内追加:
```cpp
    void rangeForCollects() {
        SeqDevice dev;
        QByteArray all;
        coro::launch([&]{
            for (QByteArray c : coro::generate(&dev)) all += c;
            coro::quit();
        });
        QTimer::singleShot(10, [&]{ dev.feed("hello "); });
        QTimer::singleShot(20, [&]{ dev.feed("world"); });
        QTimer::singleShot(30, [&]{ dev.finish(); });
        coro::exec();
        QCOMPARE(all, QByteArray("hello world"));
    }
```

- [ ] **Step 2: 运行测试,确认编译失败**

Run: `cmake --build build -j --target test_iodevice`
Expected: 编译失败 —— `io_byte_generator` 无 `begin`/`end` 成员(range-for 无法展开)。

- [ ] **Step 3: 实现 `iterator` 与 `begin()`/`end()`**

在 `include/coro/iodevice.h` 的 `io_byte_generator` 类中,`private:` 之前追加:
```cpp
    // range-for 支持：迭代器封装 next()，到达流末即等于 end()。
    class iterator {
    public:
        iterator() = default;                                   // end 哨兵
        explicit iterator(io_byte_generator* g) : g_(g) { advance(); }

        QByteArray& operator*()  { return cur_; }
        QByteArray* operator->() { return &cur_; }
        iterator& operator++() { advance(); return *this; }
        bool operator!=(const iterator& o) const { return done_ != o.done_; }
        bool operator==(const iterator& o) const { return done_ == o.done_; }
    private:
        void advance() {
            auto r = g_->next();
            if (r.closed()) { done_ = true; }
            else { cur_ = std::move(r).value(); done_ = false; }
        }
        io_byte_generator* g_ = nullptr;
        QByteArray cur_;
        bool done_ = true;
    };

    iterator begin() { return iterator(this); }
    iterator end()   { return iterator(); }
```

- [ ] **Step 4: 运行测试,确认通过**

Run:
```
cmake --build build -j --target test_iodevice && \
LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib \
./build/tests/test_iodevice rangeForCollects
```
Expected: `Totals: 1 passed, 0 failed`。

- [ ] **Step 5: 运行整个 test_iodevice,确认全绿**

Run:
```
LD_LIBRARY_PATH=/home/david/.aem/envs/david/envroot/opt/apollo/neo/packages/3rd-boost/9.0.0-alpha3-r1/lib \
./build/tests/test_iodevice
```
Expected: `Totals: 4 passed, 0 failed`。

- [ ] **Step 6: 提交**

```bash
git add include/coro/iodevice.h tests/test_iodevice.cpp
git commit -m "feat: io_byte_generator range-for 迭代器支持

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: 文档(README + CHANGELOG)与推送

**Files:**
- Modify: `README.md`(API 章节)
- Modify: `CHANGELOG.md`(`[Unreleased] / Added`)

**Interfaces:** 无代码接口;记录 Task 1–2 的 `coro::generate(QIODevice*)`。

- [ ] **Step 1: README 增补 API 条目**

在 `README.md` 的 `### API` 列表中,`await(QIODevice*)` 相关行之后追加一行:
```markdown
- `coro::generate(QIODevice*)` — 字节块**生成器**:每拉取一次挂起直到下一次 `readyRead`,产出 `readAll()` 的 `QByteArray`;流结束(`readChannelFinished`/设备销毁)正常终止(`closed()`,不抛)。支持 range-for 与显式 `next() -> result<QByteArray>` 两种消费;覆盖 QTcpSocket 等流式设备。
```

- [ ] **Step 2: CHANGELOG 增补条目**

在 `CHANGELOG.md` 的 `## [Unreleased]` 下新增一节:
```markdown
### Added — QIODevice 字节块生成器

- **`coro::generate(QIODevice*)`**(`coro/iodevice.h`,已在伞头文件中):返回 move-only 的
  `coro::io_byte_generator`。`next()` 挂起当前协程直到下一次 `readyRead`,产出 `dev->readAll()`
  的 `QByteArray`(经 `result<QByteArray>` 传递,值/关闭);每次拉取先排空 `bytesAvailable()`,
  单线程模型下无边沿丢失。`readChannelFinished` 或设备销毁时**正常**结束(`closed()`,不抛)。
  支持 range-for 与显式 `next()` 两种消费方式。底层在单次 `awaitable<int>` 上汇合
  `readyRead`/`readChannelFinished`/`destroyed`,槽内先断开全部连接以避免 rendezvous 二次 push 挂起。
- 新增测试 `test_iodevice`:`nextYieldsChunks`(逐块)、`destroyEndsClean`(销毁正常终止)、
  `rangeForCollects`(range-for 收集);采用顺序 `QIODevice` 桩,无网络依赖。
- 现有 `await(QIODevice*)` 保持不变。
```

- [ ] **Step 3: 提交**

```bash
git add README.md CHANGELOG.md
git commit -m "docs: 记录 coro::generate(QIODevice*) 字节块生成器

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 4: 推送到 origin**

Run: `git push origin master`
Expected: 推送成功。

---

## Self-Review

- **Spec coverage:** `generate()` + `io_byte_generator`(Task 1)、`next()` 三信号汇合与排空语义(Task 1 Step 3)、range-for(Task 2)、closed-不抛终止(Task 1 `destroyEndsClean`)、顺序桩三测试(Task 1–2)、README + CHANGELOG + push(Task 3)。spec 各项均有对应任务,无遗漏。`await(QIODevice*)` 保持不变(全程未触及)。
- **Placeholder scan:** 无 TBD/TODO;每个代码步骤含完整代码与确切命令、期望输出。
- **Type consistency:** `next()` 返回 `result<QByteArray>` 在 Task 1/2 一致;`generate()` 返回 `io_byte_generator` 一致;`iterator::advance()` 消费 `next()` 的 `result` 一致;`awaitable<int>` 的 `resolve(1)/resolve(0)`/`close()` 与 `result<int>::value()/closed()` 解读一致。
