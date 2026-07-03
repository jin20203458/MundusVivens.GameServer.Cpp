#include "Systems.h"
#include "Components.h"
#include "TcpServer.h"
#include "ClientSession.h"
#include "mundus_vivens.pb.h"
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <array>
#include <string_view>
#include "GrpcResultQueue.h"

// 헬퍼 함수: 에이전트 문자열 ID와 정수 ID 간의 양방향 정적 매핑
inline uint32_t GetAgentNumericId(const std::string& string_id) {
    if (string_id == "player") return 1;
    if (string_id == "npc_eva") return 2;
    if (string_id == "npc_kyle") return 3;
    if (string_id == "npc_bart") return 4;
    if (string_id == "npc_aileen") return 5;
    if (string_id == "npc_cedric") return 6;
    if (string_id == "npc_hugo") return 7;
    if (string_id == "npc_lucas") return 8;
    if (string_id == "npc_lyra") return 9;
    if (string_id == "npc_maya") return 10;
    if (string_id == "npc_valac") return 11;
    return 0;
}

inline std::string GetAgentStringId(uint32_t numeric_id) {
    if (numeric_id == 1) return "player";
    if (numeric_id == 2) return "npc_eva";
    if (numeric_id == 3) return "npc_kyle";
    if (numeric_id == 4) return "npc_bart";
    if (numeric_id == 5) return "npc_aileen";
    if (numeric_id == 6) return "npc_cedric";
    if (numeric_id == 7) return "npc_hugo";
    if (numeric_id == 8) return "npc_lucas";
    if (numeric_id == 9) return "npc_lyra";
    if (numeric_id == 10) return "npc_maya";
    if (numeric_id == 11) return "npc_valac";
    return "";
}

// 헬퍼 함수: NPC가 현재 진행 중인 활동(Activity)에 집중하여 대화를 피할지를 하드 판정
bool IsNPCFocusedOnActivity(const std::string& activity, double roll) {
    if (activity.find("취침") != std::string::npos || activity.find("휴식") != std::string::npos) {
        // 취침/휴식 중에는 대화 불가
        return true;
    }
    if (activity.find("기도") != std::string::npos || activity.find("명상") != std::string::npos) {
        // 기도/명상 중에는 80% 확률로 대화 불가
        return roll < 0.8;
    }
    // 일반 활동(산책, 노동 등)은 무조건 허용 (최종 대화 확률에서 성격/관계에 따라 확률이 결정됨)
    return false;
}

// Protobuf 메시지 직렬화 및 전송을 위한 템플릿 헬퍼 (송신 Zero-Allocation)
template <typename T>
inline bool SendProto(TcpServer& tcp, uint32_t session_index, uint16_t packet_id, const T& message) {
    thread_local static std::array<uint8_t, MAX_PACKET_SIZE> buffer;
    size_t size = message.ByteSizeLong();
    if (size > MAX_PACKET_SIZE || !message.SerializeToArray(buffer.data(), static_cast<int>(size))) {
        return false;
    }
    tcp.SendTo(session_index, packet_id, buffer.data(), size);
    return true;
}

template <typename T>
inline bool BroadcastProto(TcpServer& tcp, uint16_t packet_id, const T& message) {
    thread_local static std::array<uint8_t, MAX_PACKET_SIZE> buffer;
    size_t size = message.ByteSizeLong();
    if (size > MAX_PACKET_SIZE || !message.SerializeToArray(buffer.data(), static_cast<int>(size))) {
        return false;
    }
    tcp.BroadcastPacket(packet_id, buffer.data(), size);
    return true;
}



// 1. 감정 쇠퇴 및 감정 전염 처리 시스템
void SystemEmotionDecay(entt::registry& reg) {
    // 🆕 감정 전염 (A-1) : 각 zone 별 부정적 감정 NPC가 있는지 먼저 스캔
    std::unordered_map<uint32_t, std::vector<std::string>> zone_negative_emotions; // zone_id -> 부정적 감정 목록
    
    auto zone_view = reg.view<LocationComp, EmotionComp>();
    zone_view.each([&](LocationComp& loc, EmotionComp& emo) {
        std::string cur_emo = emo.current_emotion;
        if (cur_emo.find("분노") != std::string::npos || 
            cur_emo.find("적대") != std::string::npos || 
            cur_emo.find("공포") != std::string::npos) {
            zone_negative_emotions[loc.zone_id].push_back(cur_emo);
        }
    });

    static std::random_device contagion_rd;
    static std::mt19937 contagion_gen(contagion_rd());
    static std::uniform_real_distribution<> contagion_dis(0.0, 1.0);

    // BusyTag가 부착된 NPC는 감정 쇠퇴/전염 연산에서 배제 (O(1) 필터링)
    auto view = reg.view<EmotionComp, IdentityComp>(entt::exclude<BusyTag>);
    view.each([&](entt::entity entity, EmotionComp& emo, IdentityComp& identity) {
        // 감정 자연 쇠퇴
        if (emo.decay_ticks_remaining > 0) {
            emo.decay_ticks_remaining--;
            if (emo.decay_ticks_remaining == 0) {
                std::string old_emo = emo.current_emotion;
                std::string base_emo = emo.base_emotion;
                if (old_emo != base_emo) {
                    emo.current_emotion = base_emo;
                    std::cout << "🎭 [감정 쇠퇴] " << identity.display_name << "의 감정이 오래되어 기본 감정 [" 
                              << base_emo << "](으)로 자동 복귀되었습니다. (이전 감정: " << old_emo << ")" << std::endl;
                }
            }
        }

        // 🆕 감정 전염 적용 (불안/경계 전이)
        if (reg.all_of<LocationComp>(entity)) {
            uint32_t zone_id = reg.get<LocationComp>(entity).zone_id;
            auto it_neg = zone_negative_emotions.find(zone_id);
            if (it_neg != zone_negative_emotions.end() && !it_neg->second.empty()) {
                // 이미 평온한 상태인 경우에만 감정 전염
                if (emo.current_emotion == emo.base_emotion && 
                    emo.current_emotion != "불안" && emo.current_emotion != "경계") {
                    
                    // 마주친 부정 감정의 수 비례 확률 (개당 15%, 최대 45%)
                    float infect_chance = it_neg->second.size() * 0.15f;
                    if (infect_chance > 0.45f) infect_chance = 0.45f;

                    if (contagion_dis(contagion_gen) < infect_chance) {
                        std::string source_emo = it_neg->second[0];
                        std::string target_emo = "불안";
                        if (source_emo.find("적대") != std::string::npos) target_emo = "경계";

                        emo.current_emotion = target_emo;
                        emo.decay_ticks_remaining = 5; // 5틱 동안 지속 후 자동 쇠퇴
                        std::cout << "⚡ [감정 전염] " << identity.display_name << "이(가) 같은 구역 내 부정적 감정 [" 
                                  << source_emo << "]의 영향을 받아 [" << target_emo << "] 상태가 되었습니다!" << std::endl;
                    }
                }
            }
        }
    });
}

// 🆕 Axis 2: Job 및 Toil 상태 머신 제어 시스템
void SystemJobDriver(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    auto view = reg.view<LocationComp, ActivityComp, IdentityComp>();

    view.each([&](entt::entity entity, LocationComp& loc, ActivityComp& act, IdentityComp& identity) {
        // JobComp와 ToilComp가 없으면 기본 생성
        if (!reg.all_of<JobComp>(entity)) {
            reg.emplace<JobComp>(entity);
        }
        if (!reg.all_of<ToilComp>(entity)) {
            reg.emplace<ToilComp>(entity);
        }

        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);

        // NPC가 대화중이거나 바쁘면 Toil 상태를 Interrupted로 전환하고 대기
        if (reg.all_of<BusyTag>(entity)) {
            if (toil.state != ToilState::Interrupted) {
                // 이전에 진행 중이던 활성 Job이 있었고, 그것이 대화로 중단되었다면 Report
                if (job.is_active) {
                    std::cout << "⏸️ [Job 중단] " << identity.display_name << "의 Job " << job.job_id 
                              << "이(가) 대화(BusyTag)로 인해 강제 중단되었습니다." << std::endl;
                    
                    uint32_t npc_id = identity.npc_id;
                    uint64_t job_id = job.job_id;
                    
                    client.ReportJobStatusAsync(npc_id, job_id, 2, mundusvivens::DIALOGUE_BUSY, "대화 걸려서 바쁨 상태로 전환됨", current_tick, 
                        [&grpc_queue, entity, npc_id](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                            grpc_queue.Push([success, has_new_job, new_job, entity, npc_id](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                                if (success && inner_reg.valid(entity) && inner_reg.all_of<JobComp>(entity)) {
                                    if (has_new_job) {
                                        auto& j = inner_reg.get<JobComp>(entity);
                                        j.job_id = new_job.job_id;
                                        j.target_location = new_job.target_location;
                                        j.intent = new_job.intent;
                                        j.target_agent_id = new_job.target_agent_id;
                                        j.priority = new_job.priority;
                                        j.is_active = true;
                                        
                                        auto& t = inner_reg.get_or_emplace<ToilComp>(entity);
                                        t.state = ToilState::Idle;
                                        t.duration_ticks = 0;
                                    }
                                }
                            });
                        });
                }
                toil.state = ToilState::Interrupted;
                act.current_activity = "대화 중";
            }
            return;
        }

        // 바쁘지 않은데 상태가 Interrupted라면 Idle로 복귀
        if (toil.state == ToilState::Interrupted) {
            toil.state = ToilState::Idle;
            act.current_activity = "대기";
        }

        if (!job.is_active) {
            // 현재 활성화된 Job이 없음 -> Idle
            toil.state = ToilState::Idle;
            act.current_activity = "대기";
            return;
        }

        // Job이 있는 경우 Toil 상태 실행
        switch (toil.state) {
            case ToilState::Idle: {
                if (loc.location_name != job.target_location) {
                    toil.state = ToilState::Moving;
                    act.current_activity = "이동 중: " + job.target_location;
                } else {
                    toil.state = ToilState::Working;
                    toil.duration_ticks = 3; // 기본적으로 3틱(15초) 동안 해당 활동 진행
                    act.current_activity = job.intent;
                }
                break;
            }
            case ToilState::Moving: {
                // Axis 2에서는 즉시 텔레포트 이동 처리
                std::string old_loc = loc.location_name;
                uint32_t old_zone = loc.zone_id;
                uint32_t new_zone = grid.GetOrCreateZoneId(job.target_location);

                loc.location_name = job.target_location;
                loc.zone_id = new_zone;
                loc.x = job.target_x;
                loc.y = job.target_y;
                loc.z = job.target_z;

                if (new_zone != old_zone) {
                    grid.Move(entity, old_zone, new_zone);
                    std::cout << "🏃 [Job 이동 완료] " << identity.display_name << " 이동: [" << old_loc << "] ➔ [" 
                              << job.target_location << "] (행동: " << job.intent << ")" << std::endl;
                }

                toil.state = ToilState::Working;
                toil.duration_ticks = 3; // 기본 3틱 유지
                act.current_activity = job.intent;
                break;
            }
            case ToilState::Working: {
                if (toil.duration_ticks > 0) {
                    toil.duration_ticks--;
                    std::cout << "🛠️ [Job 진행] " << identity.display_name << " 작업 중: [" << loc.location_name 
                              << "] (남은 틱: " << toil.duration_ticks << ")" << std::endl;
                }

                if (toil.duration_ticks <= 0) {
                    // Job 완료 처리
                    std::cout << "✅ [Job 완료] " << identity.display_name << "의 Job " << job.job_id << " 완료!" << std::endl;
                    job.is_active = false;
                    toil.state = ToilState::Idle;
                    act.current_activity = "대기";

                    // C# 서버에 Job 완료 상태 보고
                    uint32_t npc_id = identity.npc_id;
                    uint64_t job_id = job.job_id;
                    client.ReportJobStatusAsync(npc_id, job_id, 0, mundusvivens::UNKNOWN_REASON, "스케줄 완료", current_tick,
                        [](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                            // 단순 완료 로그
                        });
                }
                break;
            }
            default:
                break;
        }
    });
}



// 4. 공간 그리드 기반 동일 구역 내 NPC 간 대화 트리거 시스템
void SystemSpatialDialogueTrigger(entt::registry& reg, SpatialHashGrid& grid,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis,
                                  GrpcResultQueue& grpc_queue) {
    // 공간 해시 그리드의 각 zone을 순회
    for (const auto& [zone_id, entities] : grid.AllZones()) {
        if (entities.size() < 2) continue; // 혼자 있는 구역은 건너뜀

        // 구역 내 NPC 순서를 무작위로 섞어서 앞 순서 독식 편향 제거
        std::vector<entt::entity> shuffled_entities = entities;
        std::shuffle(shuffled_entities.begin(), shuffled_entities.end(), gen);

        // 대화 가능한 후보자 필터링
        std::vector<entt::entity> candidates;
        for (auto ent : shuffled_entities) {
            if (!reg.valid(ent)) continue;
            if (reg.all_of<PlayerTag>(ent)) continue;
            if (reg.all_of<BusyTag>(ent)) continue;

            const auto& cooldown = reg.get_or_emplace<CooldownComp>(ent);
            if (tick < cooldown.global_cooldown_until) continue;
            if (cooldown.daily_dialogue_count >= 3) continue; // 일일 대화 제한

            const auto& act = reg.get<ActivityComp>(ent);
            double roll = dis(gen);
            if (IsNPCFocusedOnActivity(act.current_activity, roll)) {
                continue;
            }
            candidates.push_back(ent);
        }

        if (candidates.size() < 2) continue;

        for (size_t i = 0; i < candidates.size(); ++i) {
            entt::entity ent_a = candidates[i];
            if (!reg.valid(ent_a) || reg.all_of<BusyTag>(ent_a)) continue;

            const auto& identity_a = reg.get<IdentityComp>(ent_a);
            uint32_t id_a = identity_a.npc_id;
            std::string name_a = identity_a.display_name;

            for (size_t j = i + 1; j < candidates.size(); ++j) {
                entt::entity ent_b = candidates[j];
                if (!reg.valid(ent_b) || reg.all_of<BusyTag>(ent_b)) continue;

                const auto& identity_b = reg.get<IdentityComp>(ent_b);
                uint32_t id_b = identity_b.npc_id;
                std::string name_b = identity_b.display_name;

                // 🆕 성격(외향성) 및 관계도 캐시 확인
                float ext_a = reg.all_of<PersonalityComp>(ent_a) ? reg.get<PersonalityComp>(ent_a).extroversion : 0.5f;
                float ext_b = reg.all_of<PersonalityComp>(ent_b) ? reg.get<PersonalityComp>(ent_b).extroversion : 0.5f;

                int liking_a_to_b = 0;
                int trust_a_to_b = 50;
                if (reg.all_of<RelationshipCacheComp>(ent_a)) {
                    const auto& rels = reg.get<RelationshipCacheComp>(ent_a).relationships;
                    auto it_rel = rels.find(id_b);
                    if (it_rel != rels.end()) {
                        liking_a_to_b = it_rel->second.liking;
                        trust_a_to_b = it_rel->second.trust;
                    }
                }

                int liking_b_to_a = 0;
                int trust_b_to_a = 50;
                if (reg.all_of<RelationshipCacheComp>(ent_b)) {
                    const auto& rels = reg.get<RelationshipCacheComp>(ent_b).relationships;
                    auto it_rel = rels.find(id_a);
                    if (it_rel != rels.end()) {
                        liking_b_to_a = it_rel->second.liking;
                        trust_b_to_a = it_rel->second.trust;
                    }
                }

                // 🆕 대화 거부 (A-3) - 친밀도가 너무 나쁘면 대화를 즉시 거부
                if (liking_a_to_b < -70 || liking_b_to_a < -70) {
                    std::cout << "💔 [대화 거부] " << name_a << "와(과) " << name_b 
                              << "은(는) 서로 사이가 좋지 않아 대화를 거부했습니다. (Liking: " 
                              << liking_a_to_b << " / " << liking_b_to_a << ")" << std::endl;
                    // 즉각 쿨다운 적용
                    reg.get_or_emplace<CooldownComp>(ent_a).global_cooldown_until = tick + 10;
                    reg.get_or_emplace<CooldownComp>(ent_b).global_cooldown_until = tick + 10;
                    continue;
                }

                // 🆕 곱셈 기반 2단계 대화 및 합류 공식 도입
                // 1단계: 1:1 대화 확률 판정
                float personality_factor = 0.5f + (ext_a + ext_b) / 2.0f; // 0.5 ~ 1.5
                float affinity_factor = (liking_a_to_b + liking_b_to_a) / 2.0f; // -100 ~ +100
                float trust_factor = (trust_a_to_b + trust_b_to_a) / 2.0f; // 0 ~ 100
                
                float relationship_factor = 1.0f 
                                            + (affinity_factor / 100.0f) 
                                            + ((trust_factor - 50.0f) / 100.0f); // 0.0 ~ 3.0 (중립 1.0)
                
                constexpr float BASE_DIALOGUE_PROB = 0.08f; // 5초 틱 기준 8%
                float probability = BASE_DIALOGUE_PROB * personality_factor * relationship_factor;
                
                // [0.01, 0.50] 범위 클램프
                if (probability < 0.01f) probability = 0.01f;
                if (probability > 0.50f) probability = 0.50f;

                if (dis(gen) < probability) {
                    // 대화 대상 성립! 다자간 대화 확장 여부 검사 (최대 4인)
                    boost::container::small_vector<entt::entity, 10> group_participants = { ent_a, ent_b };
                    std::vector<uint32_t> group_ids = { id_a, id_b };

                    // 2단계: 그룹 합류 판정 (별도 확률 + 크기 감쇠)
                    constexpr float BASE_JOIN_PROB = 0.25f; // 25% 기본 합류율

                    for (size_t k = 0; k < candidates.size(); ++k) {
                        entt::entity ent_c = candidates[k];
                        if (ent_c == ent_a || ent_c == ent_b) continue;
                        if (group_participants.size() >= 4) break;
                        if (reg.all_of<BusyTag>(ent_c)) continue;

                        uint32_t id_c = reg.get<IdentityComp>(ent_c).npc_id;

                        // C와 기존 참가자들 간의 평균 호감도 및 신뢰도 계산
                        int sum_liking = 0;
                        int sum_trust = 0;
                        int member_count = 0;
                        for (auto member : group_participants) {
                            uint32_t member_id = reg.get<IdentityComp>(member).npc_id;
                            if (reg.all_of<RelationshipCacheComp>(ent_c)) {
                                const auto& rels = reg.get<RelationshipCacheComp>(ent_c).relationships;
                                auto it_rel = rels.find(member_id);
                                if (it_rel != rels.end()) {
                                    sum_liking += it_rel->second.liking;
                                    sum_trust += it_rel->second.trust;
                                    member_count++;
                                }
                            }
                        }

                        float avg_liking = member_count > 0 ? (float)sum_liking / member_count : 0.0f;
                        float avg_trust = member_count > 0 ? (float)sum_trust / member_count : 50.0f;

                        // 호감 문턱 설정: 평균 호감도가 20 이상이어야 합류 고려
                        if (avg_liking < 20.0f) continue;

                        // C의 외향성 및 관계 계수 계산
                        float ext_c = reg.all_of<PersonalityComp>(ent_c) ? reg.get<PersonalityComp>(ent_c).extroversion : 0.5f;
                        float join_personality = 0.5f + ext_c; // 0.5 ~ 1.5
                        float join_relationship = 1.0f 
                                                  + (avg_liking / 100.0f) 
                                                  + ((avg_trust - 50.0f) / 100.0f); // 0.0 ~ 3.0

                        // 그룹 크기 감쇠 적용 (2인 -> 1.0, 3인 -> 0.67)
                        float group_penalty = 2.0f / (float)group_participants.size();

                        float join_prob = BASE_JOIN_PROB * join_personality * join_relationship * group_penalty;
                        
                        // [0.00, 0.50] 범위 클램프
                        if (join_prob < 0.0f) join_prob = 0.0f;
                        if (join_prob > 0.50f) join_prob = 0.50f;

                        if (dis(gen) < join_prob) {
                            group_participants.push_back(ent_c);
                            group_ids.push_back(id_c);
                        }
                    }

                    const auto& loc_a = reg.get<LocationComp>(ent_a);
                    std::string loc_name = loc_a.location_name;

                    std::string participant_names_str = "";
                    for (auto ent : group_participants) {
                        participant_names_str += reg.get<IdentityComp>(ent).display_name + " ";
                    }

                    if (group_participants.size() > 2) {
                        std::cout << "\n👥 [다자간 C++ 공간 대화 트리거] (" << participant_names_str 
                                  << ")이(가) [" << loc_name << "]에서 그룹 대화를 시작합니다! (확률: " 
                                  << probability << ")" << std::endl;
                    } else {
                        std::cout << "\n💬 [C++ 공간 충돌 감지] " << name_a << "와(과) " << name_b 
                                  << "이(가) [" << loc_name << "] 공간에서 마주쳤습니다! (확률: " 
                                  << probability << ")" << std::endl;
                    }
                    std::cout << "💬 비동기 gRPC 통신으로 대화 트리거를 요청합니다 (완전 비동기)..." << std::endl;

                    // 중복 대화 요청 방지를 위해 즉시 임시 Busy 상태 적용
                    for (auto ent : group_participants) {
                        reg.get<ActivityComp>(ent).current_activity = "대화 요청 중";
                        reg.emplace_or_replace<BusyTag>(ent);
                    }

                    // 비동기 트리거 호출 (반응형 단일 RPC 구조)
                    client.TriggerDialogueAsync(std::move(group_ids), [&grpc_queue, group_participants, tick, loc_name, participant_names_str, &grid](bool success, const MundusVivens::DialogueResult& result) {
                        grpc_queue.Push([success, result, group_participants, tick, loc_name, participant_names_str, &grid](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {

                            bool all_valid = true;
                            for (auto ent : group_participants) {
                                if (!reg.valid(ent)) {
                                    all_valid = false;
                                    break;
                                }
                            }

                            if (success && all_valid) {
                                std::string participant_names = "";
                                for (auto ent : group_participants) {
                                    participant_names += reg.get<IdentityComp>(ent).display_name + " ";
                                }

                                std::cout << "\n🔔 [비동기 대화 완료 수신] [" << loc_name << "]에서 진행된 ("
                                          << participant_names << ")의 대화 완료!" << std::endl;

                                if (result.dialogue_lines.empty()) {
                                    std::cerr << "❌ [대화 데이터 에러] 대화 수거에 성공했으나 대사 텍스트가 비어있습니다. 에러 요약: " 
                                              << result.dialogue_summary << std::endl;
                                } else {
                                    std::cout << "\n================== [ C++ AI 대화 요약 결과 리포트 ] ==================" << std::endl;
                                    std::cout << result.dialogue_summary << std::endl;
                                    std::cout << "==============================================================" << std::endl;

                                    std::cout << "\n[실시간 소문 유통 연극 대본 로그]" << std::endl;
                                    for (const auto& line : result.dialogue_lines) {
                                        std::cout << line << std::endl;
                                    }
                                    std::cout << "==============================================================\n" << std::endl;
                                }

                                // C++ 내부 감정 상태 갱신
                                if (reg.ctx().contains<EntityIndex>()) {
                                    const auto& entity_index = reg.ctx().get<EntityIndex>();
                                    for (const auto& em_update : result.emotion_updates) {
                                        entt::entity target_ent = entt::null;
                                        auto idx_it = entity_index.by_npc_id.find(em_update.agent_id);
                                        if (idx_it != entity_index.by_npc_id.end()) {
                                            target_ent = idx_it->second;
                                        }

                                        if (reg.valid(target_ent) && reg.all_of<EmotionComp>(target_ent)) {
                                            auto& emo = reg.get<EmotionComp>(target_ent);
                                            const auto& identity = reg.get<IdentityComp>(target_ent);
                                            emo.current_emotion = em_update.new_emotion;
                                            std::cout << "🎭 [감정 동기화] " << identity.display_name << "의 감정이 [" << em_update.new_emotion 
                                                      << "](으)로 업데이트되었습니다." << std::endl;

                                            int decay_ticks = 3;
                                            std::string new_emo = em_update.new_emotion;
                                            if (new_emo.find("분노") != std::string::npos || 
                                                new_emo.find("공포") != std::string::npos || 
                                                new_emo.find("우울") != std::string::npos || 
                                                new_emo.find("슬픔") != std::string::npos || 
                                                new_emo.find("의심") != std::string::npos || 
                                                new_emo.find("경계") != std::string::npos ||
                                                new_emo.find("냉소") != std::string::npos ||
                                                new_emo.find("적대") != std::string::npos) {
                                                decay_ticks = 10;
                                            } else if (new_emo.find("기쁨") != std::string::npos || 
                                                       new_emo.find("놀람") != std::string::npos || 
                                                       new_emo.find("불안") != std::string::npos || 
                                                       new_emo.find("기대") != std::string::npos ||
                                                       new_emo.find("연민") != std::string::npos) {
                                                decay_ticks = 6;
                                            } else if (new_emo.find("흥분") != std::string::npos ||
                                                       new_emo.find("유쾌") != std::string::npos ||
                                                       new_emo.find("재미") != std::string::npos ||
                                                       new_emo.find("흥미") != std::string::npos) {
                                                decay_ticks = 3;
                                            }
                                            emo.decay_ticks_remaining = decay_ticks;
                                        }
                                    }
                                }

                                 // C++ 내부 다음 행동 계획(next_jobs) 반영
                                 if (reg.ctx().contains<EntityIndex>()) {
                                     const auto& entity_index = reg.ctx().get<EntityIndex>();
                                     for (const auto& nj : result.next_jobs) {
                                         entt::entity target_ent = entt::null;
                                         auto idx_it = entity_index.by_npc_id.find(nj.npc_id);
                                         if (idx_it != entity_index.by_npc_id.end()) {
                                             target_ent = idx_it->second;
                                         }

                                         if (reg.valid(target_ent)) {
                                             auto& job = reg.get_or_emplace<JobComp>(target_ent);
                                             job.job_id = nj.job_id;
                                             job.target_location = nj.target_location;
                                             job.target_x = nj.target_x;
                                             job.target_y = nj.target_y;
                                             job.target_z = nj.target_z;
                                             job.intent = nj.intent;
                                             job.target_agent_id = nj.target_agent_id;
                                             job.priority = nj.priority;
                                             job.is_active = true;

                                             auto& toil = reg.get_or_emplace<ToilComp>(target_ent);
                                             toil.state = ToilState::Idle;
                                             toil.duration_ticks = 0;

                                             std::cout << "🧠 [C++ 대화 후 동기화 계획 적용] " 
                                                       << reg.get<IdentityComp>(target_ent).display_name 
                                                       << " ➔ 위치: " << nj.target_location 
                                                       << " (" << nj.target_x << ", " << nj.target_y << ", " << nj.target_z << ")"
                                                       << ", 행동: " << nj.intent << " (Job: " << nj.job_id << ")" << std::endl;
                                         }
                                     }
                                 }

                                // 🆕 엿듣기(Eavesdropping) 동작성 추가
                                if (!group_participants.empty() && reg.valid(group_participants[0])) {
                                    uint32_t zone_id = reg.get<LocationComp>(group_participants[0]).zone_id;
                                    if (zone_id != 0) {
                                        const auto& bystanders = reg.ctx().contains<SpatialHashGrid>() ? 
                                            reg.ctx().get<SpatialHashGrid>().GetEntitiesInZone(zone_id) :
                                            grid.GetEntitiesInZone(zone_id);

                                        std::unordered_set<entt::entity> part_set(group_participants.begin(), group_participants.end());
                                        for (auto ent : bystanders) {
                                            if (part_set.find(ent) == part_set.end()) {
                                                if (reg.valid(ent) && reg.all_of<IdentityComp>(ent)) {
                                                    uint32_t bystander_id = reg.get<IdentityComp>(ent).npc_id;
                                                    std::string bystander_name = reg.get<IdentityComp>(ent).display_name;

                                                    uint32_t subject_id = reg.get<IdentityComp>(group_participants[0]).npc_id;

                                                    std::cout << "👂 [엿듣기 감지] " << bystander_name << "이(가) [" 
                                                              << loc_name << "]에서 일어난 대화를 엿들었습니다! 소문 주입 진행..." << std::endl;

                                                    async_client.InjectGossipAsync(bystander_id, subject_id, result.dialogue_summary, [bystander_name](bool success, const std::string& msg) {
                                                        if (success) {
                                                            std::cout << "📢 [소문 주입 성공] " << bystander_name << "의 기억에 소문이 주입되었습니다." << std::endl;
                                                        } else {
                                                            std::cerr << "❌ [소문 주입 실패] " << bystander_name << ": " << msg << std::endl;
                                                        }
                                                    });
                                                }
                                            }
                                        }
                                    }
                                }

                                // 행동 상태 갱신 및 쿨다운 설정
                                for (auto ent : group_participants) {
                                    if (reg.valid(ent)) {
                                        reg.get<CooldownComp>(ent).daily_dialogue_count++;
                                        reg.get<ActivityComp>(ent).current_activity = "대화 마침";
                                        auto& cooldown = reg.get_or_emplace<CooldownComp>(ent);
                                        cooldown.global_cooldown_until = tick + 3;
                                        if (reg.all_of<BusyTag>(ent)) reg.erase<BusyTag>(ent);
                                    }
                                }
                            } else {
                                std::cerr << "❌ [대화 요청 실패] 대화 완료 콜백 수신 실패 또는 취소됨." << std::endl;
                                for (auto ent : group_participants) {
                                    if (reg.valid(ent)) {
                                        reg.get<ActivityComp>(ent).current_activity = "대기";
                                        if (reg.all_of<BusyTag>(ent)) reg.erase<BusyTag>(ent);
                                    }
                                }
                            }
                        });
                    });

                    break; // 한 틱에 한 명은 한 번만 대화 시작 가능하도록 내부 루프 나감
                }
            }
        }
    }
}



// 6. 변경 상태 배치 gRPC 동기화 시스템
void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    std::vector<MundusVivens::AgentStatusUpdate> updates;
    std::vector<entt::entity> target_entities;
    auto view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp, LastSyncedComp>();

    view.each([&](entt::entity entity, IdentityComp& identity, LocationComp& loc, EmotionComp& emo, ActivityComp& act, LastSyncedComp& sync) {
        if (sync.location != loc.location_name || sync.emotion != emo.current_emotion || sync.activity != act.current_activity) {
            MundusVivens::AgentStatusUpdate update;
            update.agent_id = identity.npc_id;
            update.location = loc.location_name;
            update.x = loc.x;
            update.y = loc.y;
            update.z = loc.z;
            update.emotion = emo.current_emotion;
            update.activity = act.current_activity;
            updates.push_back(update);
            target_entities.push_back(entity);
        }
    });

    if (!updates.empty()) {
        // 비동기 콜백 캡처 시 std::move 적용(D-5) 및 target_entities 병렬 캡처 적용
        std::vector<MundusVivens::AgentStatusUpdate> updates_for_callback = updates; // 콜백용 명시적 복사
        client.BatchUpdateStatusAsync(std::move(updates), [&grpc_queue, target_entities = std::move(target_entities), updates = std::move(updates_for_callback)](bool success, int32_t updated_count, const std::string& message) mutable {
            grpc_queue.Push([success, updated_count, message, target_entities = std::move(target_entities), updates = std::move(updates)](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                if (success) {
                    for (size_t i = 0; i < updates.size(); ++i) {
                        entt::entity target_ent = target_entities[i];
                        if (reg.valid(target_ent) && reg.all_of<LastSyncedComp>(target_ent)) {
                            auto& sync = reg.get<LastSyncedComp>(target_ent);
                            sync.location = updates[i].location;
                            sync.emotion = updates[i].emotion;
                            sync.activity = updates[i].activity;
                        }
                    }
                    std::cout << "🔄 [gRPC-Batch 비동기 완료] " << message << " (업데이트 개수: " << updated_count << ")" << std::endl;
                } else {
                    std::cerr << "❌ [gRPC-Batch 비동기 에러] 배치 업데이트 전송 실패: " << message << std::endl;
                }
            });
        });
    }
}

// 7. 연결 끊긴 플레이어의 대화 및 좀비 엔티티 정리 시스템 구현
void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client, GrpcResultQueue& grpc_queue) {
    std::vector<entt::entity> to_cleanup;
    auto view = reg.view<PlayerTag>();
    view.each([&](entt::entity entity, const PlayerTag& tag) {
        if (tcp.GetSession(tag.session_index) == nullptr) {
            to_cleanup.push_back(entity);
        }
    });

    if (to_cleanup.empty()) return;

    auto& index = reg.ctx().get<EntityIndex>();

    for (auto player_ent : to_cleanup) {
        const auto& tag = reg.get<PlayerTag>(player_ent);
        std::cout << "⚠️ [네트워크 끊김 감지] 플레이어 세션(" << tag.session_index 
                  << ") 연결 끊김 확인. 강제 정리를 시작합니다." << std::endl;

        // 대화 중이었던 경우 대화 종료 및 NPC 해방
        if (reg.all_of<PlayerDialogueComp>(player_ent)) {
            const auto& pdc = reg.get<PlayerDialogueComp>(player_ent);
            std::cout << "  - 진행 중이던 대화(Session: " << pdc.session_id << ")를 종료하고 NPC를 해방합니다." << std::endl;

            // C# AI 서버에 종료 신호 전송 (비동기)
            async_client.EndPlayerDialogueAsync(pdc.session_id, [&grpc_queue](bool success, const std::string& summary) {
                grpc_queue.Push([success, summary](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                    std::cout << "💬 [비정상 종료 대화 정리 완료] C# AI 서버 대화 종료 응답 수신: " << summary << std::endl;
                });
            });

            // NPC 상태 정상 복귀
            if (reg.valid(pdc.npc_entity)) {
                if (reg.all_of<ActivityComp>(pdc.npc_entity)) {
                    reg.get<ActivityComp>(pdc.npc_entity).current_activity = "대기";
                }
                if (reg.all_of<BusyTag>(pdc.npc_entity)) {
                    reg.erase<BusyTag>(pdc.npc_entity);
                }
            }
        }

        // Spatial Hash Grid에서 플레이어 삭제
        if (reg.all_of<LocationComp>(player_ent)) {
            const auto& loc = reg.get<LocationComp>(player_ent);
            grid.Remove(player_ent, loc.zone_id);
        }

        // EntityIndex 맵핑 소거
        index.by_session_index.erase(tag.session_index);
        if (reg.all_of<IdentityComp>(player_ent)) {
            index.by_npc_id.erase(reg.get<IdentityComp>(player_ent).npc_id);
        }

        // 플레이어 엔티티 완전 제거 (좀비 방지)
        reg.destroy(player_ent);
        std::cout << "👤 [플레이어 제거 완료] 좀비 엔티티 및 리소스 삭제 완료." << std::endl;
    }
}

// 8. 플레이어 커맨드 처리 시스템 구현
void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick, GrpcResultQueue& grpc_queue) {
    static std::vector<PlayerCommand> local_commands;
    tcp.DrainPlayerCommands(local_commands);
    
    auto& index = reg.ctx().get<EntityIndex>();

    for (const auto& cmd : local_commands) {
        if (cmd.type == PlayerCommand::Login) {
            mundusvivens::LoginRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            // 플레이어 검색 혹은 신규 생성 (EntityIndex O(1) 활용)
            entt::entity player_ent = entt::null;
            auto it = index.by_npc_id.find(GetAgentNumericId(req.player_id()));
            if (it != index.by_npc_id.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) {
                player_ent = reg.create();
                reg.emplace<PlayerTag>(player_ent, cmd.session_index);
                reg.emplace<IdentityComp>(player_ent, GetAgentNumericId(req.player_id()), req.player_name());
                reg.emplace<LocationComp>(player_ent, 0u, "광장");
                reg.emplace<LastSyncedComp>(player_ent);

                index.by_npc_id[GetAgentNumericId(req.player_id())] = player_ent;
                index.by_session_index[cmd.session_index] = player_ent;

                std::cout << "👤 [플레이어 로그인] 신규 플레이어 등록: " << req.player_name() << " (ID: " << req.player_id() << ")" << std::endl;
            } else {
                // 재접속 시 세션 인덱스 최신화
                uint32_t old_session = reg.get<PlayerTag>(player_ent).session_index;
                index.by_session_index.erase(old_session);

                reg.get<PlayerTag>(player_ent).session_index = cmd.session_index;
                index.by_session_index[cmd.session_index] = player_ent;
                std::cout << "👤 [플레이어 재접속] 기존 플레이어 세션 갱신: " << req.player_name() << std::endl;
            }

            // 플레이어 공간 등록
            auto& loc = reg.get<LocationComp>(player_ent);
            uint32_t zone_id = grid.GetOrCreateZoneId(loc.location_name);
            loc.zone_id = zone_id;
            grid.Insert(player_ent, zone_id);

            // 로그인 응답 패킷 작성
            mundusvivens::LoginResponse resp;
            resp.set_success(true);
            resp.set_message("로그인에 성공했습니다.");

            auto* loc1 = resp.add_locations();
            loc1->set_name("광장");
            loc1->mutable_position()->set_x(0);
            loc1->mutable_position()->set_y(0);
            loc1->mutable_position()->set_z(0);

            auto* loc2 = resp.add_locations();
            loc2->set_name("여관");
            loc2->mutable_position()->set_x(-15);
            loc2->mutable_position()->set_y(0);
            loc2->mutable_position()->set_z(-5);

            auto* loc3 = resp.add_locations();
            loc3->set_name("시장");
            loc3->mutable_position()->set_x(30);
            loc3->mutable_position()->set_y(0);
            loc3->mutable_position()->set_z(10);

            auto* loc4 = resp.add_locations();
            loc4->set_name("성당");
            loc4->mutable_position()->set_x(10);
            loc4->mutable_position()->set_y(0);
            loc4->mutable_position()->set_z(50);

            // 현재 NPC 전체 상태 추가
            auto npc_view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp>(entt::exclude<PlayerTag>);
            npc_view.each([&](const IdentityComp& npc_id, const LocationComp& npc_loc, const EmotionComp& npc_emo, const ActivityComp& npc_act) {
                auto* snapshot = resp.add_npcs();
                snapshot->set_npc_id(npc_id.npc_id);
                snapshot->set_display_name(npc_id.display_name);
                auto* loc_info = snapshot->mutable_location();
                loc_info->set_name(npc_loc.location_name);
                auto* pos = loc_info->mutable_position();
                pos->set_x(npc_loc.x);
                pos->set_y(npc_loc.y);
                pos->set_z(npc_loc.z);
                snapshot->set_emotion(npc_emo.current_emotion);
                snapshot->set_activity(npc_act.current_activity);
            });

            SendProto(tcp, cmd.session_index, PacketId::SC_LOGIN_ACK, resp);
        }
        else if (cmd.type == PlayerCommand::Move) {
            mundusvivens::PlayerMoveRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent != entt::null) {
                auto& loc = reg.get<LocationComp>(player_ent);
                const auto& identity = reg.get<IdentityComp>(player_ent);
                std::string old_loc = loc.location_name;
                std::string new_loc = req.target_location().name();
                float target_x = req.target_location().position().x();
                float target_y = req.target_location().position().y();
                float target_z = req.target_location().position().z();

                uint32_t old_zone = loc.zone_id;
                uint32_t new_zone = grid.GetOrCreateZoneId(new_loc);

                loc.location_name = new_loc;
                loc.zone_id = new_zone;
                loc.x = target_x;
                loc.y = target_y;
                loc.z = target_z;
                grid.Move(player_ent, old_zone, new_zone);

                std::cout << "🏃 [TCP 플레이어 이동] 플레이어 " << identity.display_name 
                          << " 이동: [" << old_loc << "] ➔ [" << new_loc << "]" << std::endl;
            }
        }
        else if (cmd.type == PlayerCommand::TalkToNpc) {
            mundusvivens::TalkToNpcRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) continue;
            const auto& player_loc = reg.get<LocationComp>(player_ent);
            const auto& player_identity = reg.get<IdentityComp>(player_ent);

            // 대화 대상 NPC 엔티티 검색 (O(1) 해시 맵 검색)
            entt::entity npc_ent = entt::null;
            auto npc_it = index.by_npc_id.find(req.npc_id());
            if (npc_it != index.by_npc_id.end()) {
                npc_ent = npc_it->second;
            }

            if (npc_ent == entt::null) {
                std::cerr << "❌ [플레이어 대화 에러] NPC를 찾을 수 없음: " << req.npc_id() << std::endl;
                continue;
            }

            if (player_ent == npc_ent) {
                std::cerr << "❌ [플레이어 대화 에러] 자기 자신과는 대화할 수 없습니다." << std::endl;
                continue;
            }

            if (reg.all_of<PlayerTag>(npc_ent)) {
                std::cerr << "❌ [플레이어 대화 에러] 다른 플레이어와는 AI 대화를 진행할 수 없습니다." << std::endl;
                continue;
            }

            const auto& npc_loc = reg.get<LocationComp>(npc_ent);
            const auto& npc_id = reg.get<IdentityComp>(npc_ent);

            // 동일 구역인지 검증
            if (player_loc.zone_id != npc_loc.zone_id) {
                std::cerr << "❌ [플레이어 대화 에러] 플레이어와 NPC가 다른 구역에 있음. 플레이어: " 
                          << player_loc.location_name << ", NPC: " << npc_loc.location_name << std::endl;
                continue;
            }

            // NPC 대화 불가 여부 검증 (대화 중 여부 BusyTag O(1) 검사)
            if (reg.all_of<BusyTag>(npc_ent)) {
                std::cout << "⏳ [플레이어 대화 불가] NPC " << npc_id.display_name << "은(는) 이미 대화 중입니다." << std::endl;
                continue;
            }

            std::cout << "💬 [플레이어 대화 시작] 플레이어와 NPC " << npc_id.display_name << " 대화 시도..." << std::endl;

            uint32_t session_idx = cmd.session_index;
            std::string npc_name = npc_id.display_name;
            
            // NPC 임시 대화 대기 상태
            reg.get<ActivityComp>(npc_ent).current_activity = "플레이어와 대화 대기";
            reg.emplace_or_replace<BusyTag>(npc_ent);
            reg.emplace_or_replace<BusyTag>(player_ent);

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.StartPlayerDialogueAsync(GetAgentStringId(player_identity.npc_id), req.npc_id(), 
                [&grpc_queue, tcp_addr = &tcp, session_idx, npc_name, npc_ent, player_ent, session](bool success, uint64_t session_id, const std::string& greeting, const std::string& message) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, session_id, greeting, message, tcp_addr, session_idx, npc_name, npc_ent, player_ent](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        if (success) {
                            std::cout << "🚀 플레이어 대화 세션 오픈 성공: " << session_id << std::endl;
                            if (reg.valid(npc_ent)) {
                                reg.get<ActivityComp>(npc_ent).current_activity = "플레이어와 대화 중";
                            }
                            if (reg.valid(player_ent)) {
                                // PlayerDialogueComp 부착으로 대화 세션 및 NPC 매핑 (버그 G-1 방지)
                                reg.emplace_or_replace<PlayerDialogueComp>(player_ent, session_id, npc_ent);
                            }
                            mundusvivens::NpcReplyPayload reply;
                            reply.set_session_id(session_id);
                            reply.set_npc_name(npc_name);
                            reply.set_reply_text(greeting);

                            SendProto(*tcp_addr, session_idx, PacketId::SC_NPC_REPLY, reply);
                        } else {
                            std::cerr << "❌ [플레이어 대화 실패] 대화 세션을 생성하지 못했습니다. 사유: " << message << std::endl;
                            if (reg.valid(npc_ent)) {
                                reg.get<ActivityComp>(npc_ent).current_activity = "대기";
                                if (reg.all_of<BusyTag>(npc_ent)) reg.erase<BusyTag>(npc_ent);
                            }
                            if (reg.valid(player_ent) && reg.all_of<BusyTag>(player_ent)) {
                                reg.erase<BusyTag>(player_ent);
                            }
                        }
                    });
                }
            );
        }
        else if (cmd.type == PlayerCommand::PlayerMessage) {
            mundusvivens::PlayerMessageRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            uint32_t session_idx = cmd.session_index;
            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            std::string npc_name = "NPC";
            if (player_ent != entt::null && reg.all_of<PlayerDialogueComp>(player_ent)) {
                // PlayerDialogueComp에서 실제 NPC 추출하여 정확한 이름 매핑 (O(1))
                entt::entity npc_ent = reg.get<PlayerDialogueComp>(player_ent).npc_entity;
                if (reg.valid(npc_ent) && reg.all_of<IdentityComp>(npc_ent)) {
                    npc_name = reg.get<IdentityComp>(npc_ent).display_name;
                }
            }

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.SendPlayerMessageAsync(req.session_id(), req.message(),
                [&grpc_queue, tcp_addr = &tcp, session_idx, npc_name, req, session](bool success, const std::string& reply_text) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, reply_text, tcp_addr, session_idx, npc_name, req](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        if (success) {
                            mundusvivens::NpcReplyPayload reply;
                            reply.set_session_id(req.session_id());
                            reply.set_npc_name(npc_name);
                            reply.set_reply_text(reply_text);

                            SendProto(*tcp_addr, session_idx, PacketId::SC_NPC_REPLY, reply);
                        } else {
                            std::cerr << "❌ [플레이어 메시지 전송 실패] 대화 응답 전송 실패" << std::endl;
                        }
                    });
                }
            );
        }
        else if (cmd.type == PlayerCommand::EndDialogue) {
            mundusvivens::EndDialogueRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            uint32_t session_idx = cmd.session_index;
            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            entt::entity npc_ent = entt::null;
            if (player_ent != entt::null && reg.all_of<PlayerDialogueComp>(player_ent)) {
                // PlayerDialogueComp에서 정확한 NPC 엔티티를 조회하여 해제 (이전 Activity 문자열 감지 오동작 해결)
                npc_ent = reg.get<PlayerDialogueComp>(player_ent).npc_entity;
            }

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.EndPlayerDialogueAsync(req.session_id(),
                [&grpc_queue, npc_ent, player_ent, session](bool success, const std::string& summary) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, summary, npc_ent, player_ent](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        std::cout << "💬 [플레이어 대화 종료 수신] 요약: " << summary << std::endl;
                        if (reg.valid(npc_ent)) {
                            reg.get<ActivityComp>(npc_ent).current_activity = "대기";
                            if (reg.all_of<BusyTag>(npc_ent)) reg.erase<BusyTag>(npc_ent);
                        }
                        if (reg.valid(player_ent)) {
                            if (reg.all_of<BusyTag>(player_ent)) reg.erase<BusyTag>(player_ent);
                            if (reg.all_of<PlayerDialogueComp>(player_ent)) reg.erase<PlayerDialogueComp>(player_ent);
                        }
                    });
                }
            );
        }
    }
}

// 9. 월드 상태 스냅샷 클라이언트 브로드캐스트 시스템 구현
void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick) {
    mundusvivens::WorldSnapshotPayload payload;
    payload.set_tick(tick);

    auto view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp>(entt::exclude<PlayerTag>);
    view.each([&](const IdentityComp& identity, const LocationComp& loc, const EmotionComp& emo, const ActivityComp& act) {
        auto* snapshot = payload.add_npcs();
        snapshot->set_npc_id(identity.npc_id);
        snapshot->set_display_name(identity.display_name);
        auto* loc_info = snapshot->mutable_location();
        loc_info->set_name(loc.location_name);
        auto* pos = loc_info->mutable_position();
        pos->set_x(loc.x);
        pos->set_y(loc.y);
        pos->set_z(loc.z);
        snapshot->set_emotion(emo.current_emotion);
        snapshot->set_activity(act.current_activity);
    });

    BroadcastProto(tcp, PacketId::SC_WORLD_SNAPSHOT, payload);
}
