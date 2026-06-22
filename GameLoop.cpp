#include "GameLoop.h"
#include <thread>

GameLoop::GameLoop(double tick_rate_hz) {
    tick_interval_ = std::chrono::microseconds(static_cast<int64_t>(1000000.0 / tick_rate_hz));
}

void GameLoop::Run(const std::function<void(int tick)>& tick_fn, const std::atomic<bool>& keep_running) {
    using Clock = std::chrono::steady_clock;
    auto next_tick = Clock::now();
    int tick = 0;

    while (keep_running.load()) {
        tick_fn(++tick);

        next_tick += tick_interval_;
        auto now = Clock::now();
        if (now < next_tick) {
            std::this_thread::sleep_for(next_tick - now);
        } else {
            // 프레임 드랍 발생 시 타겟 타임을 현재 시간으로 맞춤
            next_tick = now;
        }
    }
}
