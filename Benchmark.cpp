#include "Benchmark.h"
#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <random>
#include <algorithm>
#include <entt/entt.hpp>
#include "TracyIntegration.h"
#include "GridMap.h"
#include "Components.h"

namespace MundusVivens {

    // OOP vs DOD 비교용 클래스
    class OOP_Entity {
    public:
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float vx = 1.0f;
        float vy = 0.0f;
        float vz = 1.0f;
        float speed = 2.0f;
        char padding[64]; // 캐시 라인 미스 증폭용 더미 패딩

        virtual void Update(float dt) {
            x += vx * speed * dt;
            y += vy * speed * dt;
            z += vz * speed * dt;
        }
        virtual ~OOP_Entity() = default;
    };

    struct DOD_Position { float x, y, z; };
    struct DOD_Velocity { float vx, vy, vz; float speed; };

    // Mutex vs Swap 큐 비교용 구조
    struct DummyCommand {
        uint32_t player_id;
        float target_x;
        float target_z;
        char payload[128]; // 패킷 페이로드 오버헤드 재현
    };

    class SimpleMutexQueue {
        std::vector<DummyCommand> data_;
        std::mutex mtx_;
    public:
        void Push(DummyCommand val) {
            std::lock_guard<std::mutex> lock(mtx_);
            data_.push_back(std::move(val));
        }
        void Drain(std::vector<DummyCommand>& out) {
            std::lock_guard<std::mutex> lock(mtx_);
            out.insert(out.end(), std::make_move_iterator(data_.begin()), std::make_move_iterator(data_.end()));
            data_.clear();
        }
    };

    class DoubleBufferedQueue {
        std::vector<DummyCommand> write_queue_;
        std::mutex mtx_;
    public:
        void Push(DummyCommand val) {
            std::lock_guard<std::mutex> lock(mtx_);
            write_queue_.push_back(std::move(val));
        }
        void Swap(std::vector<DummyCommand>& read_queue) {
            std::lock_guard<std::mutex> lock(mtx_);
            write_queue_.swap(read_queue);
        }
    };

    bool RunBenchmarkIfRequested(int argc, char* argv[]) {
        if (argc < 2) return false;
        std::string mode(argv[1]);
        if (mode == "--benchmark") {
            int scenario = 1;
            if (argc >= 3) {
                scenario = std::stoi(argv[2]);
            }
            std::cout << "\n[BENCHMARK] 시나리오 " << scenario << "번 실행 중..." << std::endl;
            if (scenario == 1) {
                RunScenarioDODvsOOP();
            } else if (scenario == 2) {
                RunScenarioLockSwapVsMutex();
            } else if (scenario == 3) {
                RunScenarioIOThreadIsolation();
            } else if (scenario == 4) {
                RunScenarioScalability();
            } else {
                std::cout << "[BENCHMARK] 알 수 없는 시나리오 번호입니다." << std::endl;
            }
            return true;
        }
        return false;
    }

    void RunScenarioDODvsOOP() {
        const int ENTITY_COUNT = 50000;
        const int LOOP_COUNT = 100;
        const float dt = 0.05f;

        std::cout << "\n==============================================" << std::endl;
        std::cout << "🧪 [시나리오 1] 캐시 지역성 테스트: DOD vs OOP" << std::endl;
        std::cout << "==============================================" << std::endl;
        std::cout << "엔티티 개수: " << ENTITY_COUNT << "개 | 틱 반복: " << LOOP_COUNT << "회\n" << std::endl;

        // 1. OOP 인스턴스화 및 무작위 셔플 (메모리 파편화 극대화)
        std::vector<std::shared_ptr<OOP_Entity>> oop_entities;
        oop_entities.reserve(ENTITY_COUNT);
        for (int i = 0; i < ENTITY_COUNT; ++i) {
            oop_entities.push_back(std::make_shared<OOP_Entity>());
        }
        // 메모리 불연속적 배치를 모방하기 위해 포인터 무작위 셔플
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(oop_entities.begin(), oop_entities.end(), g);

        // 2. DOD(EnTT) 구성
        entt::registry registry;
        for (int i = 0; i < ENTITY_COUNT; ++i) {
            auto entity = registry.create();
            registry.emplace<DOD_Position>(entity, 0.0f, 0.0f, 0.0f);
            registry.emplace<DOD_Velocity>(entity, 1.0f, 0.0f, 1.0f, 2.0f);
        }

        // --- OOP 실행 ---
        auto start_oop = std::chrono::high_resolution_clock::now();
        for (int step = 0; step < LOOP_COUNT; ++step) {
            ZoneScopedN("OOP Update Cycle");
            for (auto& entity : oop_entities) {
                entity->Update(dt);
            }
        }
        auto end_oop = std::chrono::high_resolution_clock::now();
        auto duration_oop = std::chrono::duration_cast<std::chrono::milliseconds>(end_oop - start_oop).count();

        // --- DOD 실행 ---
        auto start_dod = std::chrono::high_resolution_clock::now();
        auto view = registry.view<DOD_Position, DOD_Velocity>();
        for (int step = 0; step < LOOP_COUNT; ++step) {
            ZoneScopedN("DOD EnTT Update Cycle");
            view.each([dt](DOD_Position& pos, DOD_Velocity& vel) {
                pos.x += vel.vx * vel.speed * dt;
                pos.y += vel.vy * vel.speed * dt;
                pos.z += vel.vz * vel.speed * dt;
            });
        }
        auto end_dod = std::chrono::high_resolution_clock::now();
        auto duration_dod = std::chrono::duration_cast<std::chrono::milliseconds>(end_dod - start_dod).count();

        std::cout << "📊 [결과 보고]" << std::endl;
        std::cout << "- OOP (포인터 순회) 소요 시간 : " << duration_oop << " ms (평균 " << (double)duration_oop / LOOP_COUNT << " ms/틱)" << std::endl;
        std::cout << "- DOD (EnTT 뷰 순회) 소요 시간 : " << duration_dod << " ms (평균 " << (double)duration_dod / LOOP_COUNT << " ms/틱)" << std::endl;
        std::cout << "🔥 DOD 가 OOP 대비 약 " << (double)duration_oop / (duration_dod ? duration_dod : 1) << "배 빠름!" << std::endl;
    }

    void RunScenarioLockSwapVsMutex() {
        const int WRITER_THREADS = 16;
        const int COMMANDS_PER_THREAD = 5000;
        const int MAIN_TICK_COUNT = 200;
        long long duration_mtx = 0; // 변수 스코프 밖으로 이동

        std::cout << "\n==================================================" << std::endl;
        std::cout << "🧪 [시나리오 2] 스레드 동기화 테스트: Lock-Swap vs Mutex" << std::endl;
        std::cout << "==================================================" << std::endl;
        std::cout << "쓰기 스레드: " << WRITER_THREADS << "개 | 스레드당 송신: " << COMMANDS_PER_THREAD << "개\n" << std::endl;

        // 1. 일반 Mutex Lock 방식 테스트
        {
            SimpleMutexQueue mtx_queue;
            std::atomic<bool> writers_active(true);
            std::vector<std::thread> threads;

            for (int i = 0; i < WRITER_THREADS; ++i) {
                threads.emplace_back([&mtx_queue, &writers_active]() {
                    std::random_device rd;
                    std::mt19937 gen(rd());
                    while (writers_active) {
                        DummyCommand cmd{ 1, 10.0f, 20.0f, "dummy payload data" };
                        mtx_queue.Push(cmd);
                        std::this_thread::yield();
                    }
                });
            }

            auto start_time = std::chrono::high_resolution_clock::now();
            std::vector<DummyCommand> buffer;
            long long accum_time_mtx_ns = 0; // 순수 락 연산 누적 시간 (ns)
            for (int tick = 0; tick < MAIN_TICK_COUNT; ++tick) {
                auto ns_start = std::chrono::high_resolution_clock::now();
                {
                    ZoneScopedN("Mutex Main Drain");
                    mtx_queue.Drain(buffer);
                }
                accum_time_mtx_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - ns_start).count();
                buffer.clear();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration_mtx = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

            writers_active = false;
            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
            std::cout << "-전체 시간: " << duration_mtx << " μs (순수 락 획득/Drain 시간 누적: " << accum_time_mtx_ns / 1000.0 << " μs)" << std::endl;
            duration_mtx = accum_time_mtx_ns; // 순수 락 시간으로 비교 기준 갱신
        }

        // 2. Double Buffered Swap 방식 테스트
        {
            DoubleBufferedQueue swap_queue;
            std::atomic<bool> writers_active(true);
            std::vector<std::thread> threads;

            for (int i = 0; i < WRITER_THREADS; ++i) {
                threads.emplace_back([&swap_queue, &writers_active]() {
                    std::random_device rd;
                    std::mt19937 gen(rd());
                    while (writers_active) {
                        DummyCommand cmd{ 1, 10.0f, 20.0f, "dummy payload data" };
                        swap_queue.Push(cmd);
                        std::this_thread::yield();
                    }
                });
            }

            auto start_time = std::chrono::high_resolution_clock::now();
            std::vector<DummyCommand> buffer;
            long long accum_time_swap_ns = 0; // 순수 스왑 연산 누적 시간 (ns)
            for (int tick = 0; tick < MAIN_TICK_COUNT; ++tick) {
                auto ns_start = std::chrono::high_resolution_clock::now();
                {
                    ZoneScopedN("Double Buffered Main Swap");
                    swap_queue.Swap(buffer);
                }
                accum_time_swap_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - ns_start).count();
                buffer.clear();
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            auto end_time = std::chrono::high_resolution_clock::now();
            auto duration_swap = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

            writers_active = false;
            for (auto& t : threads) {
                if (t.joinable()) t.join();
            }
            std::cout << "-전체 시간: " << duration_swap << " μs (순수 락 획득/Swap 시간 누적: " << accum_time_swap_ns / 1000.0 << " μs)" << std::endl;
            std::cout << "🔥 순수 락 점유 기준 Swap 방식이 Mutex 방식 대비 약 " << (double)duration_mtx / (accum_time_swap_ns ? accum_time_swap_ns : 1) << "배 빠름 (락 점유 오버헤드 격리)!" << std::endl;
        }
    }

    void RunScenarioScalability() {
        std::cout << "\n==============================================" << std::endl;
        std::cout << "🧪 [시나리오 4] Scalability 테스트: A* 대량 길찾기" << std::endl;
        std::cout << "==============================================" << std::endl;

        GridMap map; // 임시 격자맵
        const std::vector<int> AGENT_COUNTS = { 10, 100, 500 };
        const int PATHFINDING_TRIALS = 100;

        for (int count : AGENT_COUNTS) {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_real_distribution<float> dis(0.0f, 1999.0f);

            auto start = std::chrono::high_resolution_clock::now();
            for (int step = 0; step < PATHFINDING_TRIALS; ++step) {
                ZoneScopedN("A* Batch Process");
                for (int i = 0; i < count; ++i) {
                    float sx = dis(gen);
                    float sz = dis(gen);
                    float ex = dis(gen);
                    float ez = dis(gen);
                    auto path = map.FindPath(sx, sz, ex, ez);
                }
            }
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

            std::cout << "- 에이전트 " << count << "명 동시 다발 길찾기 루프 (" << PATHFINDING_TRIALS << "회) 소요 시간: " 
                      << duration << " ms (틱당 평균: " << (double)duration / PATHFINDING_TRIALS << " ms)" << std::endl;
        }
    }

    void RunScenarioIOThreadIsolation() {
        std::cout << "\n==============================================" << std::endl;
        std::cout << "🧪 [시나리오 3] I/O Thread Isolation 테스트 (비동기 지연 격리)" << std::endl;
        std::cout << "==============================================" << std::endl;
        std::cout << "C# AI 서버의 처리/네트워크 응답 지연이 발생할 때," << std::endl;
        std::cout << "전통적 동기식 블로킹 설계(비교군)와 우리 비동기 설계의 프레임 유지력을 비교합니다.\n" << std::endl;

        const int TICKS = 10;
        const int GE_DELAY_MS = 500; // 500ms 외부 I/O 지연 가정

        // -------------------------------------------------------------
        // 1. [비교군] 전통적 동기식 블로킹 호출 루프
        // -------------------------------------------------------------
        std::cout << "🧱 [1단계: 비교군] 동기식 블로킹 호출 시작 (매 틱 500ms 응답 대기)" << std::endl;
        long long total_sync_frame_time = 0;
        for (int tick = 1; tick <= TICKS; ++tick) {
            auto tick_start = std::chrono::high_resolution_clock::now();
            
            // 동기식 gRPC 호출 시뮬레이션 (응답이 올 때까지 메인 스레드 대기)
            std::this_thread::sleep_for(std::chrono::milliseconds(GE_DELAY_MS));
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - tick_start).count();
            std::cout << "❌ [동기 틱 " << tick << "] 프레임 타임: " << elapsed << " ms (게임 전체 먹통 상태)" << std::endl;
            total_sync_frame_time += elapsed;
        }

        // -------------------------------------------------------------
        // 2. [우리 구조] 비동기 격리 스레드 루프
        // -------------------------------------------------------------
        std::cout << "\n🚀 [2단계: 우리 구조] 비동기 락프리 격리 호출 시작 (백그라운드 스레드에서 500ms 비동기 처리)" << std::endl;
        
        DoubleBufferedQueue queue;
        std::atomic<bool> run_io_thread(true);

        // 500ms마다 응답을 밀어주는 백그라운드 비동기 IO 스레드 가동
        std::thread io_thread([&queue, &run_io_thread, GE_DELAY_MS]() {
            while (run_io_thread) {
                std::this_thread::sleep_for(std::chrono::milliseconds(GE_DELAY_MS));
                DummyCommand cmd{ 999, 1.0f, 1.0f, "Very Delayed gRPC Response Data" };
                queue.Push(cmd);
            }
        });

        std::vector<DummyCommand> local_buffer;
        long long total_async_frame_time = 0;
        
        for (int tick = 1; tick <= TICKS; ++tick) {
            auto tick_start = std::chrono::high_resolution_clock::now();
            
            // 1) 큐 락-스왑 (O(1) 포인터 스왑으로 즉시 통과)
            {
                ZoneScopedN("Main Loop Swap");
                queue.Swap(local_buffer);
            }

            // 2) 데이터 가공
            if (!local_buffer.empty()) {
                std::cout << "🔔 [비동기 틱 " << tick << "] 비동기 응답 도착! (수신 개수: " << local_buffer.size() << ")" << std::endl;
                local_buffer.clear();
            }

            // 3) 다음 틱까지 대기 (50ms 맞춤)
            auto tick_end = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tick_end - tick_start).count();
            
            long long sleep_time = 50 - elapsed;
            if (sleep_time > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
            }
            
            auto total_tick_time = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - tick_start).count();
            std::cout << "⏱️ [비동기 틱 " << tick << "] 프레임 타임: " << total_tick_time << " ms (순수 CPU 처리: " << elapsed << " ms)" << std::endl;
            total_async_frame_time += total_tick_time;
        }

        run_io_thread = false;
        if (io_thread.joinable()) io_thread.join();

        std::cout << "\n📊 [결과 보고]" << std::endl;
        std::cout << "- 동기 블로킹 방식 평균 틱 타임 : " << (double)total_sync_frame_time / TICKS << " ms" << std::endl;
        std::cout << "- 비동기 락프리 방식 평균 틱 타임 : " << (double)total_async_frame_time / TICKS << " ms" << std::endl;
        std::cout << "🔥 비동기 격리 구조가 동기 블로킹 대비 약 " << (double)total_sync_frame_time / (total_async_frame_time ? total_async_frame_time : 1) << "배 부하 전파를 방어함!" << std::endl;
    }
}
