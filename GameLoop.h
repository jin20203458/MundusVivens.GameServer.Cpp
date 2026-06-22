#pragma once
#include <functional>
#include <chrono>
#include <atomic>

class GameLoop {
public:
    explicit GameLoop(double tick_rate_hz);
    void Run(const std::function<void(int tick)>& tick_fn, const std::atomic<bool>& keep_running);

private:
    std::chrono::microseconds tick_interval_;
};
