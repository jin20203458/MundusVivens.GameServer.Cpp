#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <map>
#include <entt/entt.hpp>
#include "MundusVivensClient.h"
#include "AsyncGrpcClient.h"
#include "GameLoop.h"
#include "Components.h"
#include "SpatialHashGrid.h"
#include "Systems.h"

// 종료 시그널 제어 플래그 및 시그널 핸들러 정의
std::atomic<bool> keep_running(true);
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        keep_running = false;
    }
}

int main() {
#ifdef _WIN32
    // Windows 환경의 콘솔창에서 한글 깨짐을 방지하기 위한 UTF-8(코드페이지 65001) 설정
    system("chcp 65001 > nul");
#endif

    // 종료 시그널 등록
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=======================================================" << std::endl;
    std::cout << "🎮 Mundus Vivens — C++ 게임 서버 시뮬레이터 콘솔 (GameLoop + AsyncGrpcClient)" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    // C# AI gRPC 서버 엔드포인트 주소 설정
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ 서버] " << server_address << " 포트의 C# AI gRPC 서버로 연결 시도 중..." << std::endl;

    // gRPC 클라이언트 채널 및 스텁 초기화 (부트스트랩 용도 동기 클라이언트)
    MundusVivens::MundusVivensClient client(server_address);
    // 비동기 통신용 클라이언트 초기화
    MundusVivens::AsyncGrpcClient async_client(server_address);
    std::cout << "[C++ 서버] gRPC 통신 채널이 성공적으로 초기화되었습니다." << std::endl;

    // C# 서버로부터 부트스트랩 데이터 동적 로드
    std::cout << "[C++ 서버] C# 서버로부터 월드 부트스트랩 데이터를 요청하는 중..." << std::endl;
    auto bootstrap = client.GetWorldBootstrap();

    if (bootstrap.locations.empty() || bootstrap.agents.empty()) {
        std::cerr << "❌ [부트스트랩 실패] 월드 초기화 데이터를 가져오지 못했습니다. 서버 상태를 확인해 주세요." << std::endl;
        return 1;
    }

    entt::registry registry;
    SpatialHashGrid spatial_grid;

    // 부트스트랩 위치 데이터 캐싱 및 Spatial Grid에 미리 Zone 생성
    for (const auto& loc : bootstrap.locations) {
        spatial_grid.GetOrCreateZoneId(loc);
    }

    // NPC 엔티티 생성
    size_t npc_count = 0;
    for (const auto& agent : bootstrap.agents) {
        if (agent.agent_id == "player") {
            continue;
        }

        auto entity = registry.create();
        
        registry.emplace<IdentityComp>(entity, agent.agent_id, agent.name);

        uint32_t zone_id = spatial_grid.GetOrCreateZoneId(agent.location);
        registry.emplace<LocationComp>(entity, zone_id, agent.location);
        spatial_grid.Insert(entity, zone_id);

        registry.emplace<ActivityComp>(entity, agent.activity);

        registry.emplace<EmotionComp>(entity, agent.emotion, agent.emotion, 0);

        registry.emplace<CooldownComp>(entity, 0, 0);

        registry.emplace<ScheduleComp>(entity);

        registry.emplace<LastSyncedComp>(entity, agent.location, agent.emotion, agent.activity);

        npc_count++;
    }

    std::cout << "[C++ 서버] 월드 부트스트랩 완료: 위치 " << bootstrap.locations.size()
              << "곳, NPC " << npc_count << "명 연동 완료." << std::endl;
    std::cout << "[C++ 서버] 20Hz 고정 틱레이트 게임 루프 가동. 종료하려면 Ctrl+C를 누르세요.\n" << std::endl;

    // 난수 생성기 및 확률 분포 세팅
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);

    int tick = 0; // 동기화 완료된 마지막 틱
    std::vector<PendingDialogue> pendingDialogues;
    std::vector<std::string> busyAgentIdsFromCSharp;
    std::map<std::string, int> dialogueCooldowns;

    bool is_first_loop = true;
    bool is_tick_sync_pending = false;

    // 20Hz (50ms) 고정 틱레이트 게임 루프 생성
    GameLoop loop(20.0);

    loop.Run([&](int physical_tick) {
        // 1. 매 프레임(50ms) 완료된 비동기 gRPC 응답 처리 및 콜백 디스패치
        async_client.DrainCompletedResults();

        // 2. 5초마다(물리 틱 100회당 1회) C# AI 서버와 논리 틱 동기화 및 ECS 시스템 구동
        if (physical_tick % 100 == 0) {
            if (!is_tick_sync_pending) {
                is_tick_sync_pending = true;
                int target_tick = tick + 1;
                std::cout << "\n================== [ C++ 월드 루프 틱 " << target_tick << " ] ==================" << std::endl;

                // 비동기로 C# 서버에 월드 틱 동기화 신호 전송
                async_client.ProcessWorldTickAsync(target_tick, [&, target_tick](bool success, const std::string& message, const std::vector<std::string>& busy_agent_ids) {
                    is_tick_sync_pending = false;
                    
                    if (success) {
                        tick = target_tick;
                        busyAgentIdsFromCSharp = busy_agent_ids;
                        std::cout << "⏱️ [틱 동기화 성공] 틱 번호 " << tick << "가 C# 서버에 동기화되었습니다. 메시지: " << message << std::endl;

                        // 감정 쇠퇴 처리
                        SystemEmotionDecay(registry, pendingDialogues, busyAgentIdsFromCSharp);

                        // 매일 아침(0시/0틱) 감지: 대화 제한 횟수 초기화 및 일일 스케줄 데이터 조회
                        if (is_first_loop || tick % 24 == 0) {
                            is_first_loop = false;
                            std::cout << "☀️ [새로운 하루 시작] NPC들의 일일 대화 제한 횟수 초기화 및 일일 스케줄 데이터 조회 중..." << std::endl;
                            // 스케줄 데이터 긁어오기는 비동기화가 불필요한 하루 1회성 동작이므로 기존 동기식 client 재활용
                            auto schedules = client.GetDailySchedules(tick);
                            if (!schedules.empty()) {
                                auto cooldown_view = registry.view<CooldownComp>();
                                cooldown_view.each([](CooldownComp& cooldown) {
                                    cooldown.daily_dialogue_count = 0;
                                });

                                auto identity_view = registry.view<IdentityComp, ScheduleComp>();
                                for (const auto& ds : schedules) {
                                    identity_view.each([&](IdentityComp& identity, ScheduleComp& sched) {
                                        if (identity.npc_id == ds.agent_id) {
                                            sched.items.clear();
                                            for (const auto& item : ds.items) {
                                                sched.items.push_back({item.start_hour, item.end_hour, item.target_location, item.activity});
                                            }
                                        }
                                    });
                                }
                                std::cout << "[C++ 서버] 일일 스케줄 갱신 완료 (대상 NPC 수: " << schedules.size() << ")" << std::endl;
                            } else {
                                std::cerr << "⚠️ [스케줄 에러] 일일 스케줄을 가져오지 못했습니다. 기존 스케줄 혹은 기본값을 유지합니다." << std::endl;
                            }
                        }

                        // NPC들의 스케줄 기반 이동 시뮬레이션
                        SystemScheduleMovement(registry, spatial_grid, tick, pendingDialogues, busyAgentIdsFromCSharp);

                        // 비동기 대화 결과 수거 및 데이터 반영
                        SystemPollDialogueResults(registry, spatial_grid, async_client, tick, pendingDialogues, dialogueCooldowns);

                        // 동일 공간 인접 검사 및 새 대화 비동기 트리거
                        SystemSpatialDialogueTrigger(registry, spatial_grid, async_client, tick, pendingDialogues, dialogueCooldowns, busyAgentIdsFromCSharp, gen, dis);

                        // 만료 쿨다운 청소 및 네트워크 동기화
                        SystemCooldownSweep(registry, tick, dialogueCooldowns);
                        SystemNetworkSync(registry, async_client);
                        
                    } else {
                        std::cerr << "❌ [틱 동기화 에러] 틱 " << target_tick << " 동기화 실패. 다음 논리 주기(5초 후)에 재시도합니다: " << message << std::endl;
                    }
                });
            }
        }
    }, keep_running);

    // -------------------------------------------------------------
    // Graceful Shutdown: 남아있는 pending 대화 NPC 상태 원복
    // -------------------------------------------------------------
    if (!pendingDialogues.empty()) {
        std::cout << "\n🧹 [종료 정리] 진행 중인 대화가 남아있어 NPC 상태를 원복합니다..." << std::endl;
        std::vector<MundusVivens::AgentStatusUpdate> shutdown_updates;
        for (const auto& pd : pendingDialogues) {
            std::string name_a = registry.valid(pd.npc_a) ? registry.get<IdentityComp>(pd.npc_a).display_name : "Unknown";
            std::string name_b = registry.valid(pd.npc_b) ? registry.get<IdentityComp>(pd.npc_b).display_name : "Unknown";

            if (registry.valid(pd.npc_a)) {
                registry.get<ActivityComp>(pd.npc_a).current_activity = "대기";
                const auto& identityA = registry.get<IdentityComp>(pd.npc_a);
                const auto& locationA = registry.get<LocationComp>(pd.npc_a);
                const auto& emotionA = registry.get<EmotionComp>(pd.npc_a);
                shutdown_updates.push_back({identityA.npc_id, locationA.location_name, emotionA.current_emotion, "대기"});
            }
            if (registry.valid(pd.npc_b)) {
                registry.get<ActivityComp>(pd.npc_b).current_activity = "대기";
                const auto& identityB = registry.get<IdentityComp>(pd.npc_b);
                const auto& locationB = registry.get<LocationComp>(pd.npc_b);
                const auto& emotionB = registry.get<EmotionComp>(pd.npc_b);
                shutdown_updates.push_back({identityB.npc_id, locationB.location_name, emotionB.current_emotion, "대기"});
            }

            std::cout << "  - " << name_a << "와(과) " << name_b << "을(를) '대기' 상태로 복원 준비." << std::endl;
        }
        if (!shutdown_updates.empty()) {
            int32_t count = 0;
            std::string msg;
            client.BatchUpdateAgentStatus(shutdown_updates, count, msg);
            std::cout << "  => [종료 정리 완료] " << msg << std::endl;
        }
    }
    std::cout << "[C++ 서버] 시뮬레이션 서버가 안전하게 종료되었습니다." << std::endl;

    return 0;
}