#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>

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
#include "LocationRegistry.h"
#include "Systems.h"
#include "BehaviorTrees.h"
#include "TcpServer.h"
#include "GridMap.h"
#include "ClientSession.h"
#include "GrpcResultQueue.h"


// MSVC STL workaround stubs moved to MSVCCompat.cpp

// 종료 시그널 제어 플래그 및 시그널 핸들러 정의
std::atomic<bool> keep_running(true);
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        keep_running = false;
    }
}

void SystemDeath(entt::registry& reg) {
    auto view = reg.view<HealthComp, IdentityComp>();
    view.each([&](entt::entity entity, HealthComp& health, IdentityComp& identity) {
        if (health.is_dead) {
            if (reg.all_of<ToilComp>(entity)) {
                auto& toil = reg.get<ToilComp>(entity);
                if (toil.state != ToilState::Idle) {
                    toil.state = ToilState::Idle;
                    toil.duration_ticks = 0;
                    toil.current_action = "Dead";
                }
            }
            if (reg.all_of<VelocityComp>(entity)) {
                auto& vel = reg.get<VelocityComp>(entity);
                vel.speed = 0.0f;
                vel.dir_x = 0.0f;
                vel.dir_z = 0.0f;
            }
            if (reg.all_of<PathfindingComp>(entity)) {
                auto& path = reg.get<PathfindingComp>(entity);
                path.waypoints.clear();
            }
            if (reg.all_of<ActivityComp>(entity)) {
                auto& act = reg.get<ActivityComp>(entity);
                if (act.current_activity != "사망") {
                    act.current_activity = "사망";
                    std::cout << "💀 [사망 처리 완료] " << identity.display_name << "의 물리 시뮬레이션이 중단되었습니다." << std::endl;
                }
            }
            if (reg.all_of<CombatComp>(entity)) {
                auto& combat = reg.get<CombatComp>(entity);
                combat.target_entity = entt::null;
            }
            if (reg.all_of<BehaviorTreeComp>(entity)) {
                reg.erase<BehaviorTreeComp>(entity);
            }
        }
    });
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

    // 2. gRPC 채널 생성 (동기/비동기 클라이언트가 공유)
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ 서버] " << server_address << " 포트의 C# AI gRPC 서버로 연결 시도 중..." << std::endl;
    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

    // 3. agrpc 컨텍스트 생성 (gRPC CompletionQueue 래핑)
    agrpc::GrpcContext grpc_ctx{std::make_unique<grpc::CompletionQueue>()};
    auto grpc_work_guard = boost::asio::make_work_guard(grpc_ctx);

    // 4. 동기식 및 비동기식 클라이언트 초기화 (공유 채널 및 컨텍스트 바인딩 주입)
    MundusVivens::MundusVivensClient client(channel);
    MundusVivens::AsyncGrpcClient async_client(channel, grpc_ctx);
    std::cout << "[C++ 서버] gRPC 통신 채널 및 리액터가 성공적으로 초기화되었습니다." << std::endl;

    // 5. gRPC 전용 백그라운드 스레드 가동 
    std::thread grpc_thread([&grpc_ctx]() { grpc_ctx.run();  });

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

    //  시뮬레이션 설정 파일 로드 및 컨텍스트에 등록
    SimulationSettings sim_settings;
    {
        std::ifstream file("shared_simulation_settings.json");
        if (!file.is_open()) {
            std::cerr << "⚠️ [Settings] shared_simulation_settings.json을 찾을 수 없어 기본 상수를 사용합니다 (Speed: 2.0, Ticks/Hour: 200)." << std::endl;
        } else {
            std::string line;
            while (std::getline(file, line)) {
                if (line.find("NPC_SPEED") != std::string::npos) {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        size_t comma = line.find(',', colon);
                        std::string val_str = line.substr(colon + 1, (comma != std::string::npos ? comma : line.size()) - colon - 1);
                        try {
                            sim_settings.npc_speed = std::stof(val_str);
                        } catch (...) {
                            std::cerr << "❌ [Settings 에러] NPC_SPEED 파싱 실패" << std::endl;
                        }
                    }
                } else if (line.find("TICKS_PER_GAME_HOUR") != std::string::npos) {
                    size_t colon = line.find(':');
                    if (colon != std::string::npos) {
                        size_t comma = line.find(',', colon);
                        std::string val_str = line.substr(colon + 1, (comma != std::string::npos ? comma : line.size()) - colon - 1);
                        try {
                            sim_settings.ticks_per_game_hour = std::stoi(val_str);
                        } catch (...) {
                            std::cerr << "❌ [Settings 에러] TICKS_PER_GAME_HOUR 파싱 실패" << std::endl;
                        }
                    }
                }
            }
            std::cout << "⚙️ [Settings] shared_simulation_settings.json 로드 완료: NPC_SPEED = " 
                      << sim_settings.npc_speed << ", TICKS_PER_GAME_HOUR = " << sim_settings.ticks_per_game_hour << std::endl;
        }
    }

    //  C#-C++ 설정 동등성(Handshake Validation) 검증
    if (std::abs(sim_settings.npc_speed - bootstrap.npc_speed) > 0.001f || 
        sim_settings.ticks_per_game_hour != static_cast<int>(bootstrap.ticks_per_game_hour)) {
        std::cerr << "\n====================================================" << std::endl;
        std::cerr << "❌ [FATAL] [시뮬레이션 설정 불일치 감지] C++ 게임서버와 C# AI서버의 물리 상수가 일치하지 않습니다!" << std::endl;
        std::cerr << "   - C++ 로컬 설정: NPC_SPEED = " << sim_settings.npc_speed 
                  << ", TICKS_PER_GAME_HOUR = " << sim_settings.ticks_per_game_hour << std::endl;
        std::cerr << "   - C# 원격 설정: NPC_SPEED = " << bootstrap.npc_speed 
                  << ", TICKS_PER_GAME_HOUR = " << bootstrap.ticks_per_game_hour << std::endl;
        std::cerr << "📢 [조치 사항] C++ 측 shared_simulation_settings.json 파일이 빌드 과정에서 동기화되지 않았거나 복사 오류일 가능성이 있습니다." << std::endl;
        std::cerr << "====================================================\n" << std::endl;
        
        grpc_ctx.stop();
        if (grpc_thread.joinable()) {
            grpc_thread.join();
        }
        return 1; // 기동 중단 (Fatal Assertion Crash)
    }

    registry.ctx().emplace<SimulationSettings>(sim_settings);

    LocationRegistry location_registry;
    GridMap grid_map;
    grid_map.LoadMap(bootstrap.locations);

    // EntityIndex 등록  ( 역방향 탐색용 해쉬맵 싱글톤 )
    auto& entity_index = registry.ctx().emplace<EntityIndex>();

    //  동적 ID 매핑 및 감정 레지스트리 컨텍스트 등록
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
        
        EmotionCategory cat = EmotionCategory::Neutral;
        if (keyword == "분노") cat = EmotionCategory::Anger;
        else if (keyword == "적대") cat = EmotionCategory::Hostility;
        else if (keyword == "공포") cat = EmotionCategory::Fear;
        emotion_registry.category_table.push_back(cat);
        
        next_emo_id++;
    }

    // 기본 플레이어 매핑 등록
    id_mapper.string_to_numeric["player"] = 1;
    id_mapper.numeric_to_string[1] = "player";

    // 부트스트랩 데이터 컨텍스트에 등록 (로그인 시 클라이언트에 전달하기 위함)
    registry.ctx().emplace<MundusVivens::WorldBootstrapData>(bootstrap);

    // 부트스트랩 위치 데이터 캐싱 및 위치 등록기에 거점 등록
    for (const auto& loc : bootstrap.locations) {
        location_registry.RegisterLocation(
            loc.name,
            loc.x,
            loc.z,
            static_cast<LocationType>(loc.type),
            loc.region_id,
            loc.territory_id
        );
    }

    // 2D Region Bake Map 생성 (O(1) 거점 판정 최적화)
    location_registry.BakeRegionMap(GridMap::WIDTH, GridMap::HEIGHT);

    //  부트스트랩 가구(사물) 데이터 로드 및 엔티티 생성
    std::cout << "[C++ 서버] " << bootstrap.furniture.size() << "개의 가구/사물 오브젝트 데이터를 동적 배치하는 중..." << std::endl;
    for (const auto& furn : bootstrap.furniture) {
        auto furn_entity = registry.create();
        
        registry.emplace<IdentityComp>(furn_entity, 0u, furn.name);
        
        registry.emplace<LocationComp>(furn_entity, 0u, furn.parent_location, furn.x, furn.y, furn.z);
        
        AffordanceType aff_type = static_cast<AffordanceType>(furn.type);
        auto& aff = registry.emplace<AffordanceComp>(furn_entity, aff_type, entt::null);
        aff.is_temporary = furn.is_temporary;
        
        // 가구를 LocationRegistry에 좌표 기반으로 등록
        location_registry.UpdateEntityPosition(furn_entity, furn.x, furn.z, registry);
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

        //  동적 에이전트 ID 매핑 테이블 빌드
        id_mapper.numeric_to_string[agent.agent_id] = agent.string_id;
        
        // 헬퍼 람다: 대소문자 변환 시 변화가 있는 경우(영문 대문자 포함 등)에만 2차 등록
        auto register_alias = [&](const std::string& alias) {
            if (alias.empty()) return;
            id_mapper.string_to_numeric[alias] = agent.agent_id;
            
            std::string lower = alias;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower != alias) {
                id_mapper.string_to_numeric[lower] = agent.agent_id;
            }
        };

        register_alias(agent.string_id);
        register_alias(agent.name);

        auto& loc_comp = registry.emplace<LocationComp>(entity, 0u, agent.location, agent.x, agent.y, agent.z);
        location_registry.UpdateEntityPosition(entity, agent.x, agent.z, registry);

        registry.emplace<ActivityComp>(entity, agent.activity);

        //  감정 정수 ID 동적 매핑 및 초기화
        if (emotion_registry.name_to_id.find(agent.emotion) == emotion_registry.name_to_id.end()) {
            emotion_registry.name_to_id[agent.emotion] = next_emo_id;
            emotion_registry.decay_ticks_table.push_back(3); // 기본 3틱
            
            EmotionCategory cat = EmotionCategory::Neutral;
            if (agent.emotion.find("분노") != std::string::npos) cat = EmotionCategory::Anger;
            else if (agent.emotion.find("적대") != std::string::npos) cat = EmotionCategory::Hostility;
            else if (agent.emotion.find("공포") != std::string::npos) cat = EmotionCategory::Fear;
            emotion_registry.category_table.push_back(cat);
            
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

        //  생체 욕구 컴포넌트 추가 및 난수(50~90) 스태거링
        auto& needs = registry.emplace<NeedsComp>(entity);
        std::random_device rd_needs;
        std::mt19937 gen_needs(rd_needs());
        std::uniform_real_distribution<float> dist_needs(50.0f, 90.0f);
        needs.hunger = dist_needs(gen_needs);
        needs.fatigue = dist_needs(gen_needs);

        // 🆕 체력 및 전투 컴포넌트 추가
        registry.emplace<HealthComp>(entity);
        registry.emplace<CombatComp>(entity);

        // 🆕 4단계: 충동-억제 관련 컴포넌트들 초기화
        registry.emplace<AggroComp>(entity);
        auto& faction = registry.emplace<FactionComp>(entity);
        registry.emplace<SentienceComp>(entity, true); // NPC는 기본적으로 지성체

        // 몬스터 시뮬레이션을 위한 팩션 세팅
        if (agent.name.find("늑대") != std::string::npos || agent.name.find("Wolf") != std::string::npos) {
            faction.faction_name = "Wolf";
            registry.get<SentienceComp>(entity).is_sentient = false; // 늑대는 비지성체 (즉각 덮침)
        } else if (agent.name.find("고블린") != std::string::npos || agent.name.find("Goblin") != std::string::npos) {
            faction.faction_name = "Goblin";
            registry.get<SentienceComp>(entity).is_sentient = true;  // 고블린은 지성체
        } else if (agent.name.find("도적") != std::string::npos || agent.name.find("Bandit") != std::string::npos) {
            faction.faction_name = "Bandit";
        } else {
            faction.faction_name = "Human";
        }

        //  행동 트리(BT) 컴포넌트 추가 및 초기화
        auto& bt = registry.emplace<BehaviorTreeComp>(entity);
        bt.root_node = BT::CreateSurvivalTree();

        registry.emplace<LastSyncedComp>(entity, agent.location, agent.emotion, agent.activity);

        //  성격 및 관계 데이터 초기 연동
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

    //  행동 트리(BT) 실행을 위한 전역 컨텍스트 등록
    registry.ctx().emplace<BT::BTContext>(&async_client, &grpc_queue, &location_registry, &tick, &grid_map);

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
        SystemCleanupDisconnectedPlayerDialogues(registry, location_registry, tcp_server, async_client, grpc_queue);

        // 3. 매 프레임(50ms)마다 플레이어 패킷 명령어 즉각 처리 (이동, 대화 메시지 등)
        SystemPlayerCommands(registry, location_registry, tcp_server, async_client, tick, grpc_queue);

        // 4. 틱 동기화가 완료된 경우, 메인 프레임 동기화 타이밍에 맞추어 모든 시스템 결정론적 순차 실행 (Task G)
        if (tick_synced_ready.exchange(false)) {
            tick = next_tick_val;

            
            std::cout << "⏱️ [틱 동기화 완료] 틱 번호 " << tick << "가 C# 서버에 동기화되었습니다. 메시지: " << temp_message << std::endl;

            //  관계 변동치(RelationshipDelta) 반영
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

            // C# 서버에 Pending Job 요청
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
                                    job.target_x = job_payload.target_x;
                                    job.target_y = job_payload.target_y;
                                    job.target_z = job_payload.target_z;
                                    job.intent = job_payload.intent;
                                    job.target_agent_id = job_payload.target_agent_id;
                                    job.priority = job_payload.priority;
                                    job.category = static_cast<JobCategory>(job_payload.category);
                                    job.is_active = true;

                                    auto& toil = inner_reg.get_or_emplace<ToilComp>(target_ent);
                                    toil.state = ToilState::Idle;
                                    toil.duration_ticks = 0;

                                    //  새 스케줄(Job)이 수신되었으므로, 대기 중이던 ScheduleWait BusyTag 해제
                                    if (inner_reg.all_of<BusyTag>(target_ent)) {
                                        auto& busy = inner_reg.get<BusyTag>(target_ent);
                                        if (busy.reason == BusyReason::ScheduleWait) {
                                            inner_reg.erase<BusyTag>(target_ent);
                                        }
                                    }
                                }
                            }
                        }
                    }
                });
            });

            // Job 상태 머신 구동
            SystemJobDriver(registry, location_registry, tick, async_client, grpc_queue);

            // 동일 공간 인접 검사 및 새 대화 비동기 트리거
            SystemSocialInteraction(registry, location_registry, tcp_server, async_client, tick, gen, dis, grpc_queue);

            // 네트워크 동기화
            SystemNetworkSync(registry, async_client, grpc_queue);
        }

        // 경로 탐색 및 실시간 이동 구동 (20Hz)
        SystemBusyAmbient(registry, 0.05f);
        SystemPerception(registry, location_registry); // 🆕 4단계: 어그로 및 위협 감지 갱신 (20Hz)
        SystemBehaviorTree(registry); //  행동 트리(BT) 엔진 실행
        SystemDeath(registry);        //  🆕 사망 처리 시스템
        SystemSurvivalOverride(registry, location_registry, tick, async_client, grpc_queue); //  생체 위기 감지 및 인터럽트
        SystemPathfinding(registry, grid_map);
        SystemMovement(registry, location_registry, grid_map, tick);
        SystemAffordanceResolver(registry, location_registry, tick, async_client, grpc_queue); //  가구 무결성 검증 및 자동 반납 전용

        // 월드 상태 스냅샷 클라이언트 브로드캐스트 (20Hz)
        SystemBroadcastWorldSnapshot(registry, tcp_server, tick);

        int ticks_per_hour = 200;
        if (registry.ctx().contains<SimulationSettings>()) {
            ticks_per_hour = registry.ctx().get<SimulationSettings>().ticks_per_game_hour;
        }

        // 4. C# AI 서버와 논리 틱 동기화 요청
        if (physical_tick % ticks_per_hour == 0) {
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
    busy_view.each([&](BusyTag& busy, const IdentityComp& identity, const LocationComp& location, const EmotionComp& emotion, ActivityComp& activity) {
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