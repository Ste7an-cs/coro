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

} // namespace coro
