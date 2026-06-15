#pragma once
#include <functional>
#include <chrono>

namespace coro {
int  exec();
void quit(int code = 0);
void launch(std::function<void()> fn);
void yield();
void sleep(std::chrono::milliseconds ms);
}
