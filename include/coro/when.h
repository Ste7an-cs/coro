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
    // 各 Task 已各自在独立 fiber 并发运行；顺序 get() 仍只等到“最慢”那个完成。
    return std::tuple<Ts...>(ts.get()...);
}

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

} // namespace coro
