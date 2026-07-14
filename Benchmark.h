#pragma once

namespace MundusVivens {
    // 벤치마크 실행 진입점. cmd 인자가 있으면 동작하고 true 반환.
    bool RunBenchmarkIfRequested(int argc, char* argv[]);

    // 시나리오 1: DOD vs OOP 캐시 효율성 측정
    void RunScenarioDODvsOOP();

    // 시나리오 2: Lock-Swap vs Mutex Lock 경합 측정
    void RunScenarioLockSwapVsMutex();

    // 시나리오 3: I/O Thread Isolation (네트워크 비동기 지연 격리)
    void RunScenarioIOThreadIsolation();

    // 시나리오 4: Scalability (A* 부하 테스트)
    void RunScenarioScalability();
}
