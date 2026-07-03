#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <entt/entt.hpp>
#include <agrpc/asio_grpc.hpp>
#include "MundusVivensClient.h"
#include "AsyncGrpcClient.h"
#include "GameLoop.h"
#include "Components.h"
#include "SpatialHashGrid.h"
#include "Systems.h"
#include "TcpServer.h"
#include "GridMap.h"
#include "ClientSession.h"
#include "GrpcResultQueue.h"


// MSVC 17.14+ STL vectorization linker mismatch workaround stubs
#if defined(_MSC_VER) && _MSC_VER < 1940
#include <cstdint>
extern "C" {
    size_t __stdcall __std_find_first_not_of_trivial_pos_1(
        const void* _Haystack, size_t _Haystack_length, const void* _Needle, size_t _Needle_length) noexcept 
    {
        const char* haystack = static_cast<const char*>(_Haystack);
        const char* needle = static_cast<const char*>(_Needle);
        for (size_t i = 0; i < _Haystack_length; ++i) {
            bool found = false;
            for (size_t j = 0; j < _Needle_length; ++j) {
                if (haystack[i] == needle[j]) {
                    found = true;
                    break;
                }
            }
            if (!found) return i;
        }
        return _Haystack_length;
    }

    const void* __stdcall __std_min_element_8i(const void* _First, const void* _Last) noexcept {
        const int64_t* first = static_cast<const int64_t*>(_First);
        const int64_t* last = static_cast<const int64_t*>(_Last);
        if (first == last) return first;
        const int64_t* min_el = first;
        while (++first != last) {
            if (*first < *min_el) {
                min_el = first;
            }
        }
        return min_el;
    }

    const void* __stdcall __std_max_element_d_(const void* _First, const void* _Last) noexcept {
        const double* first = static_cast<const double*>(_First);
        const double* last = static_cast<const double*>(_Last);
        if (first == last) return first;
        const double* max_el = first;
        while (++first != last) {
            if (*max_el < *first) {
                max_el = first;
            }
        }
        return max_el;
    }
}
#endif

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
    std::cout << "🎮 Mundus Vivens — C++ 게임 서버 시뮬레이터 콘솔 (3-스레드 멀티 리액터)" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    // 1. Boost.Asio I/O 서비스 초기화 및 네트워크 전용 스레드 가동
    boost::asio::io_context io;
    auto io_work_guard = boost::asio::make_work_guard(io);
    std::thread io_thread([&io]() { io.run(); });

    // 2. gRPC 채널 생성 (동기/비동기 클라이언트가 공유 - Task D)
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ 서버] " << server_address << " 포트의 C# AI gRPC 서버로 연결 시도 중..." << std::endl;
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

    // 3. agrpc 컨텍스트 생성 (gRPC CompletionQueue 래핑 - Task E)
    agrpc::GrpcContext grpc_ctx{std::make_unique<grpc::CompletionQueue>()};
    auto grpc_work_guard = boost::asio::make_work_guard(grpc_ctx);

    // 4. 동기식 및 비동기식 클라이언트 초기화 (공유 채널 및 컨텍스트 바인딩 주입)
    MundusVivens::MundusVivensClient client(channel);
    MundusVivens::AsyncGrpcClient async_client(channel, grpc_ctx);
    std::cout << "[C++ 서버] gRPC 통신 채널 및 리액터가 성공적으로 초기화되었습니다." << std::endl;

    // 5. gRPC 전용 백그라운드 리액터 스레드 가동 (폴링 없이 네이티브 블로킹 대기 - Task E)
    std::thread grpc_thread([&]() {
        grpc_ctx.run(); // Completion Queue 이벤트를 네이티브 블로킹으로 대기 (유휴 시 CPU 0% 점유)
    });

    // 6. TCP 서버 초기화 (포트 7777)
    TcpServer tcp_server(io, 7777);
    tcp_server.Start();

    // C# 서버로부터 부트스트랩 데이터 동적 로드
    std::cout << "[C++ 서버] C# 서버로부터 월드 부트스트랩 데이터를 요청하는 중..." << std::endl;
    auto bootstrap = client.GetWorldBootstrap();

    if (bootstrap.locations.empty() || bootstrap.agents.empty()) {
        std::cerr << "❌ [부트스트랩 실패] 월드 초기화 데이터를 가져오지 못했습니다. 서버 상태를 확인해 주세요." << std::endl;
        grpc_ctx.stop();
        if (grpc_thread.joinable()) {
            grpc_thread.join();
        }
        return 1;
    }

    entt::registry registry;
    SpatialHashGrid spatial_grid;
    GridMap grid_map;
    grid_map.LoadMap();

    // EntityIndex 등록 (O(1) 역방향 탐색용 싱글톤 리소스 - Task B)
    auto& entity_index = registry.ctx().emplace<EntityIndex>();

    // 🆕 동적 ID 매핑 및 감정 레지스트리 컨텍스트 등록
    auto& id_mapper = registry.ctx().emplace<AgentIdMapper>();
    auto& emotion_registry = registry.ctx().emplace<EmotionRegistry>();

    // 기본 감정들과 쇠퇴 규칙들을 레지스트리에 등록
    std::vector<std::pair<std::string, int32_t>> base_rules = {
        {"분노", 10}, {"공포", 10}, {"우울", 10}, {"슬픔", 10}, {"의심", 10}, {"경계", 10}, {"냉소", 10}, {"적대", 10},
        {"기쁨", 6}, {"놀람", 6}, {"불안", 6}, {"기대", 6}, {"연민", 6},
        {"흥분", 3}, {"유쾌", 3}, {"재미", 3}, {"흥미", 3},
        {"평온", 0}, {"대기", 0}
    };

    uint8_t next_emo_id = 0;
    for (const auto& [keyword, ticks] : base_rules) {
        emotion_registry.name_to_id[keyword] = next_emo_id;
        emotion_registry.decay_ticks_table.push_back(ticks);
        next_emo_id++;
    }

    // 기본 플레이어 매핑 등록
    id_mapper.string_to_numeric["player"] = 1;
    id_mapper.numeric_to_string[1] = "player";

    // 부트스트랩 위치 데이터 캐싱 및 Spatial Grid에 미리 Zone 생성
    for (const auto& loc : bootstrap.locations) {
        spatial_grid.GetOrCreateZoneId(loc);
    }

    // NPC 엔티티 생성
    size_t npc_count = 0;
    for (const auto& agent : bootstrap.agents) {
        if (agent.agent_id == 1) {
            continue;
        }

        auto entity = registry.create();
        
        registry.emplace<IdentityComp>(entity, agent.agent_id, agent.name);
        // 역방향 ID 인덱스 맵 등록
        entity_index.by_npc_id[agent.agent_id] = entity;

        // 🆕 동적 에이전트 ID 매핑 테이블 빌드
        id_mapper.numeric_to_string[agent.agent_id] = agent.name;
        id_mapper.string_to_numeric[agent.name] = agent.agent_id;
        
        // 영어 식별자 접두사 매핑도 지원 (예: "npc_eva")
        std::string lower_name = agent.name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        std::string english_id = "npc_" + lower_name;
        id_mapper.string_to_numeric[english_id] = agent.agent_id;
        id_mapper.string_to_numeric[lower_name] = agent.agent_id;

        uint32_t zone_id = spatial_grid.GetOrCreateZoneId(agent.location);
        registry.emplace<LocationComp>(entity, zone_id, agent.location, agent.x, agent.y, agent.z);
        spatial_grid.Insert(entity, zone_id);

        registry.emplace<ActivityComp>(entity, agent.activity);

        // 🆕 감정 정수 ID 동적 매핑 및 초기화
        if (emotion_registry.name_to_id.find(agent.emotion) == emotion_registry.name_to_id.end()) {
            emotion_registry.name_to_id[agent.emotion] = next_emo_id;
            emotion_registry.decay_ticks_table.push_back(3); // 기본 3틱
            next_emo_id++;
        }

        auto& emo = registry.emplace<EmotionComp>(entity);
        emo.current_emotion = agent.emotion;
        emo.base_emotion = agent.emotion;
        emo.current_emotion_id = emotion_registry.name_to_id[agent.emotion];
        emo.base_emotion_id = emo.current_emotion_id;
        emo.decay_ticks_remaining = 0;

        auto& cooldown = registry.emplace<CooldownComp>(entity);
        cooldown.max_social_energy = static_cast<int32_t>(50.0f + agent.extroversion * 100.0f);
        cooldown.social_energy = cooldown.max_social_energy;

        registry.emplace<JobComp>(entity);
        registry.emplace<ToilComp>(entity);

        registry.emplace<LastSyncedComp>(entity, agent.location, agent.emotion, agent.activity);

        // 🆕 성격 및 관계 데이터 초기 연동
        registry.emplace<PersonalityComp>(entity, agent.extroversion);
        auto& rel_cache = registry.emplace<RelationshipCacheComp>(entity);
        for (const auto& rel_snap : agent.relationships) {
            RelationshipEntry entry;
            entry.liking = rel_snap.liking;
            entry.trust = rel_snap.trust;
            rel_cache.relationships[rel_snap.target_agent_id] = entry;
        }

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
    GrpcResultQueue grpc_queue;

    bool is_first_loop = true;
    bool is_tick_sync_pending = false;

    // 결정론적(Determinism) 업데이트 조율용 플래그 및 캐시 변수 (Task G)
    std::atomic<bool> tick_synced_ready(false);
    std::vector<MundusVivens::RelationshipDelta> temp_relationship_deltas;
    std::string temp_message;
    int next_tick_val = 0;

    // 20Hz (50ms) 고정 틱레이트 게임 루프 생성
    GameLoop loop(20.0);

    loop.Run([&](int physical_tick) {
        // 1. gRPC 비동기 결과를 메인 스레드에서 안전하게 실행
        grpc_queue.Drain(registry, tcp_server, async_client);

        // 2. 연결 끊김 감지 및 대화 즉각 정리 시스템
        SystemCleanupDisconnectedPlayerDialogues(registry, spatial_grid, tcp_server, async_client, grpc_queue);

        // 3. 매 프레임(50ms)마다 플레이어 패킷 명령어 즉각 처리 (이동, 대화 메시지 등)
        SystemPlayerCommands(registry, spatial_grid, tcp_server, async_client, tick, grpc_queue);

        // 4. 틱 동기화가 완료된 경우, 메인 프레임 동기화 타이밍에 맞추어 모든 시스템 결정론적 순차 실행 (Task G)
        if (tick_synced_ready.exchange(false)) {
            tick = next_tick_val;

            
            std::cout << "⏱️ [틱 동기화 완료] 틱 번호 " << tick << "가 C# 서버에 동기화되었습니다. 메시지: " << temp_message << std::endl;

            // 🆕 관계 변동치(RelationshipDelta) 반영
            for (const auto& delta : temp_relationship_deltas) {
                auto it = entity_index.by_npc_id.find(delta.from_agent_id);
                if (it != entity_index.by_npc_id.end()) {
                    auto entity = it->second;
                    auto& rel_cache = registry.get_or_emplace<RelationshipCacheComp>(entity);
                    auto& entry = rel_cache.relationships[delta.to_agent_id];
                    entry.liking = delta.liking;
                    entry.trust = delta.trust;
                    std::cout << "🤝 [관계 동기화] " << delta.from_agent_id << " -> " << delta.to_agent_id 
                              << " (Liking: " << entry.liking << ", Trust: " << entry.trust << ")" << std::endl;
                }
            }



            // 감정 쇠퇴 처리
            SystemEmotionDecay(registry);

            // 매일 아침(0시/0틱) 감지: 대화 제한 횟수 초기화
            if (is_first_loop || tick % 24 == 0) {
                is_first_loop = false;
                std::cout << "☀️ [새로운 하루 시작] NPC들의 사회적 에너지 완전 충전 및 상대별 쿨다운 리셋" << std::endl;
                auto cooldown_view = registry.view<CooldownComp>();
                cooldown_view.each([](CooldownComp& cooldown) {
                    cooldown.social_energy = cooldown.max_social_energy;
                    cooldown.cognitive_refractory_until = 0;
                    cooldown.cooldown_per_target.clear();
                });
            }

            // 🚀 Axis 2: C# 서버에 Pending Job 요청
            async_client.GetPendingJobsAsync(tick, [&grpc_queue, &entity_index](bool success, const std::vector<MundusVivens::MundusVivensClient::JobPayload>& jobs) {
                grpc_queue.Push([success, jobs, &entity_index](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                    if (success) {
                        for (const auto& job_payload : jobs) {
                            auto idx_it = entity_index.by_npc_id.find(job_payload.npc_id);
                            if (idx_it != entity_index.by_npc_id.end()) {
                                entt::entity target_ent = idx_it->second;
                                if (inner_reg.valid(target_ent)) {
                                    auto& job = inner_reg.get_or_emplace<JobComp>(target_ent);
                                    if (job.job_id == job_payload.job_id && job.is_active) {
                                        continue;
                                    }
                                    job.job_id = job_payload.job_id;
                                    job.target_location = job_payload.target_location;
                                    job.intent = job_payload.intent;
                                    job.target_agent_id = job_payload.target_agent_id;
                                    job.priority = job_payload.priority;
                                    job.is_active = true;

                                    auto& toil = inner_reg.get_or_emplace<ToilComp>(target_ent);
                                    toil.state = ToilState::Idle;
                                    toil.duration_ticks = 0;
                                }
                            }
                        }
                    }
                });
            });

            // 🚀 Axis 2: Job 상태 머신 구동
            SystemJobDriver(registry, spatial_grid, tick, async_client, grpc_queue);

            // 🚀 Axis 3: 경로 탐색 및 실시간 이동 구동
            SystemPathfinding(registry, grid_map);
            SystemMovement(registry, spatial_grid, tick);

            // 동일 공간 인접 검사 및 새 대화 비동기 트리거
            SystemSocialInteraction(registry, spatial_grid, async_client, tick, gen, dis, grpc_queue);

            // 네트워크 동기화
            SystemNetworkSync(registry, async_client, grpc_queue);

            // 월드 상태 스냅샷 클라이언트 브로드캐스트
            SystemBroadcastWorldSnapshot(registry, tcp_server, tick);
        }

        // 4. 5초마다(물리 틱 100회당 1회) C# AI 서버와 논리 틱 동기화 요청
        if (physical_tick % 100 == 0) {
            if (!is_tick_sync_pending) {
                is_tick_sync_pending = true;
                int target_tick = tick + 1;
                std::cout << "\n================== [ C++ 월드 루프 틱 " << target_tick << " ] ==================" << std::endl;

                // 비동기로 C# 서버에 월드 틱 동기화 신호 전송
                async_client.ProcessWorldTickAsync(target_tick, [&grpc_queue, &is_tick_sync_pending, &next_tick_val, &temp_relationship_deltas, &temp_message, &tick_synced_ready, target_tick](bool success, const std::string& message, const std::vector<uint32_t>& busy_agent_ids, const std::vector<MundusVivens::RelationshipDelta>& relationship_deltas) {
                    grpc_queue.Push([success, message, relationship_deltas, target_tick, &is_tick_sync_pending, &next_tick_val, &temp_relationship_deltas, &temp_message, &tick_synced_ready](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        is_tick_sync_pending = false;
                        if (success) {
                            // 결과만 캐시에 임시 저장하고, 메인 게임 루프 틱 스케줄 단계에서 결정론적으로 반영하도록 함 (Task G)
                            next_tick_val = target_tick;
                            temp_relationship_deltas = relationship_deltas;
                            temp_message = message;
                            tick_synced_ready = true;
                        } else {
                            std::cerr << "❌ [틱 동기화 에러] 틱 " << target_tick << " 동기화 실패. 다음 논리 주기(5초 후)에 재시도합니다: " << message << std::endl;
                        }
                    });
                });
            }
        }
    }, keep_running);


    // -------------------------------------------------------------
    // Graceful Shutdown: 남아있는 pending 대화 NPC 상태 원복 및 스레드 자원 정리
    // -------------------------------------------------------------
    io_work_guard.reset();
    io.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }

    grpc_ctx.stop();
    if (grpc_thread.joinable()) {
        grpc_thread.join();
    }

    auto busy_view = registry.view<BusyTag, IdentityComp, LocationComp, EmotionComp, ActivityComp>();
    std::vector<MundusVivens::AgentStatusUpdate> shutdown_updates;
    std::string participant_names = "";
    busy_view.each([&](const IdentityComp& identity, const LocationComp& location, const EmotionComp& emotion, ActivityComp& activity) {
        if (activity.current_activity == "대화 요청 중" || activity.current_activity == "플레이어와 대화 중" || activity.current_activity == "플레이어와 대화 대기") {
            participant_names += identity.display_name + " ";
            activity.current_activity = "대기";
            
            MundusVivens::AgentStatusUpdate update;
            update.agent_id = identity.npc_id;
            update.location = location.location_name;
            update.x = location.x;
            update.y = location.y;
            update.z = location.z;
            update.emotion = emotion.current_emotion;
            update.activity = "대기";
            shutdown_updates.push_back(update);
        }
    });
    if (!shutdown_updates.empty()) {
        std::cout << "\n🧹 [종료 정리] 진행 중인 대화가 남아있어 NPC 상태를 원복합니다..." << std::endl;
        std::cout << "  - (" << participant_names << ")을(를) '대기' 상태로 복원 준비." << std::endl;
        int32_t count = 0;
        std::string msg;
        client.BatchUpdateAgentStatus(shutdown_updates, count, msg);
        std::cout << "  => [종료 정리 완료] " << msg << std::endl;
    }
    std::cout << "[C++ 서버] 시뮬레이션 서버가 안전하게 종료되었습니다." << std::endl;

    return 0;
}