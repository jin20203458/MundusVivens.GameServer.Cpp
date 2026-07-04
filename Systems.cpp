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

// 헬퍼 함수: 에이전트 문자열 ID와 정수 ID 간의 양방향 매핑 (컨텍스트 레지스트리 참조)
inline uint32_t GetAgentNumericId(const entt::registry& reg, const std::string& string_id) {
    if (reg.ctx().contains<AgentIdMapper>()) {
        const auto& mapper = reg.ctx().get<AgentIdMapper>();
        auto it = mapper.string_to_numeric.find(string_id);
        if (it != mapper.string_to_numeric.end()) {
            return it->second;
        }
    }
    return 0;
}

inline std::string GetAgentStringId(const entt::registry& reg, uint32_t numeric_id) {
    if (reg.ctx().contains<AgentIdMapper>()) {
        const auto& mapper = reg.ctx().get<AgentIdMapper>();
        auto it = mapper.numeric_to_string.find(numeric_id);
        if (it != mapper.numeric_to_string.end()) {
            return it->second;
        }
    }
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
                    emo.current_emotion_id = emo.base_emotion_id; // 🆕 정수 ID 동기화
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

                        // 🆕 감정 전염 시 정수 ID 도 동적 매핑
                        if (reg.ctx().contains<EmotionRegistry>()) {
                            const auto& registry = reg.ctx().get<EmotionRegistry>();
                            auto it_emo = registry.name_to_id.find(target_emo);
                            if (it_emo != registry.name_to_id.end()) {
                                emo.current_emotion_id = it_emo->second;
                            }
                        }

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
                // 실시간 A* 이동(Axis 3)에서 처리를 전담하므로, 이미 목적지에 도달한 경우에만 Working으로 전환
                if (loc.location_name == job.target_location) {
                    toil.state = ToilState::Working;
                    toil.duration_ticks = 3;
                    act.current_activity = job.intent;
                }
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



// 장소별 사회적 맥락 가중치 반환 헬퍼
// 장소별 사회적 맥락 가중치 반환 헬퍼
float GetLocationSocialModifier(const std::string& location_name) {
    if (location_name.find("Tavern") != std::string::npos || location_name.find("술집") != std::string::npos) {
        return 1.8f;
    } else if (location_name.find("Market") != std::string::npos || location_name.find("시장") != std::string::npos) {
        return 1.5f;
    } else if (location_name.find("Square") != std::string::npos || location_name.find("광장") != std::string::npos) {
        return 1.2f;
    } else if (location_name.find("Church") != std::string::npos || location_name.find("성당") != std::string::npos) {
        return 0.3f;
    }
    return 1.0f;
}

// 4. 공간 그리드 기반 P2P 소셜 인터랙션 시스템 (주도자-응답자 모델)
void SystemSocialInteraction(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                             MundusVivens::AsyncGrpcClient& client, int tick,
                             std::mt19937& gen, std::uniform_real_distribution<>& dis,
                             GrpcResultQueue& grpc_queue) {
    
    // 매 틱마다 모든 NPC의 사회적 에너지를 미세하게 회복
    auto recovery_view = reg.view<CooldownComp, LocationComp>();
    recovery_view.each([tick](CooldownComp& cooldown, LocationComp& loc) {
        // Tavern/술집에 있으면 매 틱 1 회복. 일반 장소에서는 10틱에 1 회복
        bool in_tavern = (loc.location_name.find("Tavern") != std::string::npos || loc.location_name.find("술집") != std::string::npos);
        if (in_tavern) {
            if (cooldown.social_energy < cooldown.max_social_energy) {
                cooldown.social_energy = std::min(cooldown.max_social_energy, cooldown.social_energy + 1);
            }
        } else {
            if (tick % 10 == 0 && cooldown.social_energy < cooldown.max_social_energy) {
                cooldown.social_energy = std::min(cooldown.max_social_energy, cooldown.social_energy + 1);
            }
        }
    });

    constexpr float BASE_INITIATION_PROB = 0.15f; // 기본 주도 확률 (15%)
    constexpr float MAX_DIALOGUE_DISTANCE = 20.0f; // 최대 대화 가능 거리

    for (const auto& [zone_id, entities] : grid.AllZones()) {
        if (entities.size() < 2) continue; // 혼자 있는 구역은 건너뜀

        // 대화 가능한 후보자 필터링
        std::vector<entt::entity> candidates;
        for (auto ent : entities) {
            if (!reg.valid(ent)) continue;
            if (reg.all_of<PlayerTag>(ent)) continue;
            if (reg.all_of<BusyTag>(ent)) continue;

            const auto& cooldown = reg.get_or_emplace<CooldownComp>(ent);
            if (cooldown.social_energy < 20) continue; // 사회적 에너지 고갈 상태면 대화 제외 (최소 20 필요)
            if (tick <= cooldown.cognitive_refractory_until) continue; // 인지적 불응기 상태면 대화 제외

            const auto& act = reg.get<ActivityComp>(ent);
            double roll = dis(gen);
            if (IsNPCFocusedOnActivity(act.current_activity, roll)) continue;
            
            candidates.push_back(ent);
        }

        if (candidates.size() < 2) continue;

        // 구역 내 NPC 순서를 무작위로 섞음
        std::shuffle(candidates.begin(), candidates.end(), gen);

        // 구역의 대표 장소 가중치 계산 (첫 번째 유효 엔티티의 위치 사용)
        std::string zone_loc_name = reg.get<LocationComp>(candidates[0]).location_name;
        float location_modifier = GetLocationSocialModifier(zone_loc_name);

        bool dialogue_triggered_in_zone = false;

        // 주도자(Initiator) 개별 판정
        for (entt::entity initiator : candidates) {
            if (reg.all_of<BusyTag>(initiator)) continue; // 이미 엮였을 수 있음

            auto& init_cooldown = reg.get_or_emplace<CooldownComp>(initiator);
            if (tick <= init_cooldown.last_initiative_tick) continue; // 스팸 방지 (이번 틱에는 한 번만 시도)

            float ext_i = reg.all_of<PersonalityComp>(initiator) ? reg.get<PersonalityComp>(initiator).extroversion : 0.5f;

            // 주도 확률 계산 (개인 속성만으로 결정)
            float initiation_prob = BASE_INITIATION_PROB * (0.3f + ext_i) * location_modifier;
            if (initiation_prob < 0.02f) initiation_prob = 0.02f;
            if (initiation_prob > 0.60f) initiation_prob = 0.60f;

            if (dis(gen) >= initiation_prob) continue; // 주도 실패

            init_cooldown.last_initiative_tick = tick; // 시도 기록

            // 타깃 선택 (가중 랜덤)
            const auto& loc_i = reg.get<LocationComp>(initiator);
            uint32_t id_i = reg.get<IdentityComp>(initiator).npc_id;

            std::vector<std::pair<entt::entity, float>> potential_targets;
            float total_weight = 0.0f;

            for (entt::entity target_candidate : candidates) {
                if (target_candidate == initiator) continue;
                if (reg.all_of<BusyTag>(target_candidate)) continue;

                uint32_t id_c = reg.get<IdentityComp>(target_candidate).npc_id;

                // 상대별 쿨다운 체크
                auto it_cooldown = init_cooldown.cooldown_per_target.find(id_c);
                if (it_cooldown != init_cooldown.cooldown_per_target.end() && tick < it_cooldown->second) {
                    continue; // 이 상대와는 아직 쿨다운 중
                }

                // 거리 필터 (좌표 기반)
                const auto& loc_c = reg.get<LocationComp>(target_candidate);
                float dx = loc_i.x - loc_c.x;
                float dz = loc_i.z - loc_c.z;
                float dist = std::sqrt(dx*dx + dz*dz);
                if (dist > MAX_DIALOGUE_DISTANCE) continue;

                // 호감도 조회
                int liking_i_to_c = 0;
                if (reg.all_of<RelationshipCacheComp>(initiator)) {
                    const auto& rels = reg.get<RelationshipCacheComp>(initiator).relationships;
                    auto it_rel = rels.find(id_c);
                    if (it_rel != rels.end()) liking_i_to_c = it_rel->second.liking;
                }
                
                int liking_c_to_i = 0;
                if (reg.all_of<RelationshipCacheComp>(target_candidate)) {
                    const auto& rels = reg.get<RelationshipCacheComp>(target_candidate).relationships;
                    auto it_rel = rels.find(id_i);
                    if (it_rel != rels.end()) liking_c_to_i = it_rel->second.liking;
                }

                // 대화 거부 (원수 관계)
                if (liking_i_to_c < -70 || liking_c_to_i < -70) continue;

                // 호감도 기반 가중치 계산
                float weight = std::max(1.0f, (float)(liking_i_to_c + 60));
                potential_targets.push_back({target_candidate, weight});
                total_weight += weight;
            }

            if (potential_targets.empty()) continue; // 타깃 없음

            // 가중 랜덤으로 타깃 하나 선택
            float rand_weight = dis(gen) * total_weight;
            entt::entity target = entt::null;
            for (const auto& pt : potential_targets) {
                rand_weight -= pt.second;
                if (rand_weight <= 0.0f) {
                    target = pt.first;
                    break;
                }
            }
            if (target == entt::null) target = potential_targets.back().first; // 안전 장치

            // 타깃 수락 판정
            uint32_t id_t = reg.get<IdentityComp>(target).npc_id;
            float ext_t = reg.all_of<PersonalityComp>(target) ? reg.get<PersonalityComp>(target).extroversion : 0.5f;
            
            int liking_t_to_i = 0;
            if (reg.all_of<RelationshipCacheComp>(target)) {
                const auto& rels = reg.get<RelationshipCacheComp>(target).relationships;
                auto it_rel = rels.find(id_i);
                if (it_rel != rels.end()) liking_t_to_i = it_rel->second.liking;
            }

            float accept_prob = 0.5f + (ext_t * 0.25f) + (liking_t_to_i / 200.0f) + ((location_modifier - 1.0f) * 0.15f);
            if (accept_prob < 0.10f) accept_prob = 0.10f;
            if (accept_prob > 0.95f) accept_prob = 0.95f;

            if (dis(gen) >= accept_prob) {
                // 수락 거절
                std::cout << "💔 [대화 수락 거절] " << reg.get<IdentityComp>(target).display_name 
                          << "이(가) " << reg.get<IdentityComp>(initiator).display_name << "의 대화 요청을 거절했습니다." << std::endl;
                // 짧은 쿨다운 (다음에 또 거절당하는 빈도를 줄임)
                init_cooldown.cooldown_per_target[id_t] = tick + 3;
                continue;
            }

            // 대화 성립! 다자간 합류 판정
            boost::container::small_vector<entt::entity, 10> group_participants = { initiator, target };
            std::vector<uint32_t> group_ids = { id_i, id_t };

            constexpr float BASE_JOIN_PROB = 0.25f;

            for (entt::entity other : candidates) {
                if (other == initiator || other == target) continue;
                if (group_participants.size() >= 4) break;
                if (reg.all_of<BusyTag>(other)) continue;

                uint32_t id_o = reg.get<IdentityComp>(other).npc_id;

                int sum_liking = 0;
                int sum_trust = 0;
                int member_count = 0;
                for (auto member : group_participants) {
                    uint32_t member_id = reg.get<IdentityComp>(member).npc_id;
                    if (reg.all_of<RelationshipCacheComp>(other)) {
                        const auto& rels = reg.get<RelationshipCacheComp>(other).relationships;
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

                if (avg_liking < 20.0f) continue;

                float ext_o = reg.all_of<PersonalityComp>(other) ? reg.get<PersonalityComp>(other).extroversion : 0.5f;
                float join_personality = 0.5f + ext_o;
                float join_relationship = 1.0f + (avg_liking / 100.0f) + ((avg_trust - 50.0f) / 100.0f);
                float group_penalty = 2.0f / (float)group_participants.size();

                float join_prob = BASE_JOIN_PROB * join_personality * join_relationship * group_penalty;
                if (join_prob < 0.0f) join_prob = 0.0f;
                if (join_prob > 0.50f) join_prob = 0.50f;

                if (dis(gen) < join_prob) {
                    group_participants.push_back(other);
                    group_ids.push_back(id_o);
                }
            }

            std::string participant_names_str = "";
            for (auto ent : group_participants) {
                participant_names_str += reg.get<IdentityComp>(ent).display_name + " ";
            }

            if (group_participants.size() > 2) {
                std::cout << "\n👥 [다자간 그룹 대화 성립] (" << participant_names_str 
                          << ")이(가) [" << zone_loc_name << "]에서 그룹 대화를 시작합니다!" << std::endl;
            } else {
                std::cout << "\n💬 [1:1 대화 성립] " << reg.get<IdentityComp>(initiator).display_name << " ➔ " 
                          << reg.get<IdentityComp>(target).display_name 
                          << " [" << zone_loc_name << "]에서 대화를 주도했습니다." << std::endl;
            }
            std::cout << "💬 비동기 gRPC 통신으로 대화 트리거를 요청합니다..." << std::endl;

            // 중복 대화 요청 방지를 위해 즉시 임시 Busy 상태 적용
            for (auto ent : group_participants) {
                reg.get<ActivityComp>(ent).current_activity = "대화 요청 중";
                reg.emplace_or_replace<BusyTag>(ent);
            }

            // [브로드캐스트] 유니티 클라이언트에 대화 시작 전달
            {
                mundusvivens::DialogueEventPayload start_payload;
                start_payload.set_task_id(0);
                if (group_participants.size() > 0 && reg.all_of<IdentityComp>(group_participants[0])) {
                    start_payload.set_npc_a_name(reg.get<IdentityComp>(group_participants[0]).display_name);
                }
                if (group_participants.size() > 1 && reg.all_of<IdentityComp>(group_participants[1])) {
                    start_payload.set_npc_b_name(reg.get<IdentityComp>(group_participants[1]).display_name);
                }
                start_payload.set_is_started(true);
                auto* start_loc = start_payload.mutable_location();
                start_loc->set_name(zone_loc_name);
                if (reg.all_of<LocationComp>(initiator)) {
                    const auto& init_loc = reg.get<LocationComp>(initiator);
                    auto* start_pos = start_loc->mutable_position();
                    start_pos->set_x(init_loc.x);
                    start_pos->set_y(init_loc.y);
                    start_pos->set_z(init_loc.z);
                }
                BroadcastProto(tcp, PacketId::SC_DIALOGUE_EVENT, start_payload);
            }
            // [비동기 gRPC 호출]
            client.TriggerDialogueAsync(std::move(group_ids), 
                [&grpc_queue, group_participants, tick, zone_loc_name, &grid](bool success, const MundusVivens::DialogueResult& result) {
                    grpc_queue.Push([success, result, group_participants, tick, zone_loc_name, &grid](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        
                        bool all_valid = true;
                        for (auto ent : group_participants) {
                            if (!reg.valid(ent)) {
                                all_valid = false;
                                break;
                            }
                        }

                        if (success && !result.has_error && all_valid) {
                            std::string participant_names = "";
                            for (auto ent : group_participants) {
                                participant_names += reg.get<IdentityComp>(ent).display_name + " ";
                            }

                            std::cout << "\n🔔 [비동기 대화 완료 수신] [" << zone_loc_name << "]에서 진행된 ("
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

                                // [브로드캐스트] 유니티 클라이언트에 대화 종료 및 스크립트 대사 정보 전송
                                {
                                    mundusvivens::DialogueEventPayload end_payload;
                                    end_payload.set_task_id(result.task_id);
                                    if (group_participants.size() > 0 && reg.all_of<IdentityComp>(group_participants[0])) {
                                        end_payload.set_npc_a_name(reg.get<IdentityComp>(group_participants[0]).display_name);
                                    }
                                    if (group_participants.size() > 1 && reg.all_of<IdentityComp>(group_participants[1])) {
                                        end_payload.set_npc_b_name(reg.get<IdentityComp>(group_participants[1]).display_name);
                                    }
                                    end_payload.set_is_started(false);
                                    end_payload.set_summary(result.dialogue_summary);
                                    
                                    auto* end_loc = end_payload.mutable_location();
                                    end_loc->set_name(zone_loc_name);
                                    if (group_participants.size() > 0 && reg.all_of<LocationComp>(group_participants[0])) {
                                        const auto& end_pos_comp = reg.get<LocationComp>(group_participants[0]);
                                        auto* end_pos = end_loc->mutable_position();
                                        end_pos->set_x(end_pos_comp.x);
                                        end_pos->set_y(end_pos_comp.y);
                                        end_pos->set_z(end_pos_comp.z);
                                    }

                                    for (const auto& s_line : result.structured_lines) {
                                        auto* new_line = end_payload.add_lines();
                                        new_line->set_speaker_id(s_line.speaker_id);
                                        new_line->set_speaker_name(s_line.speaker_name);
                                        new_line->set_text(s_line.text);
                                    }

                                    BroadcastProto(tcp, PacketId::SC_DIALOGUE_EVENT, end_payload);
                                }
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

                                        // 2. C#에서 전송된 동적 강도(intensity)에 따른 쇠퇴 틱수 결정
                                        int decay_ticks = 3;
                                        std::string intensity_str = "MEDIUM";

                                        if (em_update.intensity == 1) { // LOW
                                            decay_ticks = 3;
                                            intensity_str = "LOW";
                                        } else if (em_update.intensity == 3) { // HIGH
                                            decay_ticks = 10;
                                            intensity_str = "HIGH";
                                         } else { // MEDIUM (2) or default
                                             decay_ticks = 6;
                                             intensity_str = "MEDIUM";
                                         }

                                        // 1. 레지스트리 상의 고유 ID 매핑 처리 (O(1) 인덱스 및 동적 등록 유지)
                                        if (reg.ctx().contains<EmotionRegistry>()) {
                                            auto& emo_reg = reg.ctx().get<EmotionRegistry>();
                                            std::cout << "🎭 [감정 동기화] " << identity.display_name << "의 감정이 [" << em_update.new_emotion 
                                                      << "](으)로 업데이트되었습니다. 강도: [" << intensity_str << "] (" << decay_ticks << " 틱 유지)" << std::endl;
                                            auto it_emo = emo_reg.name_to_id.find(em_update.new_emotion);
                                            if (it_emo == emo_reg.name_to_id.end()) {
                                                uint8_t next_id = static_cast<uint8_t>(emo_reg.decay_ticks_table.size());
                                                emo_reg.name_to_id[em_update.new_emotion] = next_id;
                                                emo_reg.decay_ticks_table.push_back(decay_ticks);
                                                emo.current_emotion_id = next_id;
                                            } else {
                                                emo.current_emotion_id = it_emo->second;
                                            }
                                        }
                                        emo.decay_ticks_remaining = decay_ticks;
                                    }
                                }

                                // C++ 내부 다음 행동 계획(next_jobs) 반영
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
                                                          << zone_loc_name << "]에서 일어난 대화를 엿들었습니다! 소문 주입 진행..." << std::endl;

                                                async_client.InjectBeliefAsync(bystander_id, subject_id, result.dialogue_summary, mundusvivens::ProtoBeliefType::BELIEF_TYPE_OVERHEARD, [bystander_name](bool success, const std::string& msg) {
                                                    if (success) {
                                                        std::cout << "📢 [믿음(소문) 주입 성공] " << bystander_name << "의 기억에 엿들은 정보가 주입되었습니다." << std::endl;
                                                    } else {
                                                        std::cerr << "❌ [믿음(소문) 주입 실패] " << bystander_name << ": " << msg << std::endl;
                                                    }
                                                });
                                            }
                                        }
                                    }
                                }
                            }

                            // 행동 상태 갱신 및 상대별 쿨다운/사회적 에너지 소모 설정
                            for (auto ent : group_participants) {
                                if (reg.valid(ent)) {
                                    auto& cooldown = reg.get_or_emplace<CooldownComp>(ent);
                                    cooldown.social_energy = std::max(0, cooldown.social_energy - 30); // 🆕 대화 시 사회적 에너지 30 소모
                                    cooldown.cognitive_refractory_until = tick + 3;                    // 🆕 인지적 불응기 3틱 설정
                                    reg.get<ActivityComp>(ent).current_activity = "생각 정리 중";
                                    
                                    // 같이 대화한 멤버들에 대해서만 쿨다운 설정 (예: 6틱)
                                    for (auto other : group_participants) {
                                        if (other == ent) continue;
                                        uint32_t other_id = reg.get<IdentityComp>(other).npc_id;
                                        cooldown.cooldown_per_target[other_id] = tick + 6;
                                    }

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

            dialogue_triggered_in_zone = true;
            break; // 한 zone에서 이번 틱에는 한 쌍만 대화를 트리거하고 넘어감
        } // end of initiator loop
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
            auto it = index.by_npc_id.find(GetAgentNumericId(reg, req.player_id()));
            if (it != index.by_npc_id.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) {
                player_ent = reg.create();
                reg.emplace<PlayerTag>(player_ent, cmd.session_index);
                reg.emplace<IdentityComp>(player_ent, GetAgentNumericId(reg, req.player_id()), req.player_name());
                reg.emplace<LocationComp>(player_ent, 0u, "광장");
                reg.emplace<LastSyncedComp>(player_ent);

                index.by_npc_id[GetAgentNumericId(reg, req.player_id())] = player_ent;
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

            if (reg.ctx().contains<MundusVivens::WorldBootstrapData>())
            {
                const auto& bootstrap = reg.ctx().get<MundusVivens::WorldBootstrapData>();
                for (const auto& loc : bootstrap.locations)
                {
                    auto* new_loc = resp.add_locations();
                    new_loc->set_name(loc.name);
                    auto* pos = new_loc->mutable_position();
                    pos->set_x(loc.x);
                    pos->set_y(loc.y);
                    pos->set_z(loc.z);
                }
            }

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

            async_client.StartPlayerDialogueAsync(GetAgentStringId(reg, player_identity.npc_id), req.npc_id(), 
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
    view.each([&](entt::entity entity, const IdentityComp& identity, const LocationComp& loc, const EmotionComp& emo, const ActivityComp& act) {
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

        if (reg.all_of<VelocityComp>(entity)) {
            const auto& vel = reg.get<VelocityComp>(entity);
            auto* vel_vec = snapshot->mutable_velocity();
            vel_vec->set_x(vel.dir_x * vel.speed);
            vel_vec->set_y(0.0f);
            vel_vec->set_z(vel.dir_z * vel.speed);
        }
    });

    BroadcastProto(tcp, PacketId::SC_WORLD_SNAPSHOT, payload);
}

// 🆕 Axis 3: 경로 탐색 처리 시스템 (A* 호출)
void SystemPathfinding(entt::registry& reg, const GridMap& map) {
    auto view = reg.view<JobComp, ToilComp, LocationComp>();
    view.each([&](entt::entity entity, JobComp& job, ToilComp& toil, LocationComp& loc) {
        if (toil.state == ToilState::Moving) {
            if (!reg.all_of<PathfindingComp>(entity)) {
                reg.emplace<PathfindingComp>(entity);
            }
            auto& pathfinding = reg.get<PathfindingComp>(entity);

            // 경로가 비어있는 경우 새로 계산
            if (pathfinding.waypoints.empty()) {
                float target_x = job.target_x;
                float target_z = job.target_z;

                // target_location 이름이 있고 좌표가 (0,0)인 경우 사전 조회
                if (!job.target_location.empty() && target_x == 0.0f && target_z == 0.0f) {
                    map.GetLocationCoords(job.target_location, target_x, target_z);
                    job.target_x = target_x;
                    job.target_z = target_z;
                }

                // A* 길찾기 수행
                pathfinding.waypoints = map.FindPath(loc.x, loc.z, target_x, target_z);
                pathfinding.current_waypoint_index = 0;

                if (!pathfinding.waypoints.empty()) {
                    if (!reg.all_of<VelocityComp>(entity)) {
                        reg.emplace<VelocityComp>(entity);
                    }
                    auto& vel = reg.get<VelocityComp>(entity);
                    vel.speed = 2.0f; // 기본 속도 2.0 m/s
                    
                    if (reg.all_of<IdentityComp>(entity)) {
                        std::cout << "🧭 [경로 생성] " << reg.get<IdentityComp>(entity).display_name 
                                  << "이(가) [" << job.target_location << "] (" << target_x << ", " << target_z 
                                  << ")로의 경로를 탐색하여 " << pathfinding.waypoints.size() << "개의 노드를 찾았습니다." << std::endl;
                    }
                } else {
                    // 경로 탐색 실패 또는 이미 도달함 -> 대기 상태 전환
                    toil.state = ToilState::Idle;
                    if (reg.all_of<VelocityComp>(entity)) {
                        auto& vel = reg.get<VelocityComp>(entity);
                        vel.dir_x = 0.0f;
                        vel.dir_z = 0.0f;
                        vel.speed = 0.0f;
                    }
                }
            }
        }
    });
}

// 🆕 Axis 3: 실시간 20Hz 이동 처리 시스템
void SystemMovement(entt::registry& reg, SpatialHashGrid& grid, int tick) {
    constexpr float dt = 0.05f; // 20Hz 기준 dt

    auto view = reg.view<LocationComp, ToilComp, PathfindingComp, VelocityComp, JobComp, IdentityComp>();
    view.each([&](entt::entity entity, LocationComp& loc, ToilComp& toil, PathfindingComp& path, VelocityComp& vel, JobComp& job, IdentityComp& identity) {
        if (toil.state == ToilState::Moving) {
            if (path.current_waypoint_index >= path.waypoints.size()) {
                // 더 이상 갈 노드가 없음 -> 목적지 도착
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
                    std::cout << "🏃 [실시간 이동 완료] " << identity.display_name << " 이동: [" << old_loc << "] ➔ [" 
                              << job.target_location << "] (행동: " << job.intent << ")" << std::endl;
                }

                toil.state = ToilState::Working;
                toil.duration_ticks = 3;
                toil.current_action = job.intent;
                
                auto& act = reg.get<ActivityComp>(entity);
                act.current_activity = job.intent;

                vel.dir_x = 0.0f;
                vel.dir_z = 0.0f;
                vel.speed = 0.0f;
                path.waypoints.clear();
                path.current_waypoint_index = 0;
                
                std::cout << "🏁 [목적지 도착] " << identity.display_name 
                          << "이(가) 목적지에 도착하여 작업을 시작합니다." << std::endl;
                return;
            }

            // 현재 타겟 웨이포인트 획득
            const auto& target = path.waypoints[path.current_waypoint_index];

            // 벡터 계산
            float dx = target.x - loc.x;
            float dz = target.z - loc.z;
            float dist = std::sqrt(dx * dx + dz * dz);

            if (dist < 0.1f) {
                // 노드 근접 도달 -> 다음 노드로
                path.current_waypoint_index++;
                return;
            }

            // 방향 벡터 정규화
            vel.dir_x = dx / dist;
            vel.dir_z = dz / dist;

            // 이동 거리
            float move_step = vel.speed * dt;

            if (move_step >= dist) {
                // 이번 프레임에 노드에 완전 도달
                loc.x = target.x;
                loc.z = target.z;
                path.current_waypoint_index++;
            } else {
                // 일반 이동
                loc.x += vel.dir_x * move_step;
                loc.z += vel.dir_z * move_step;
            }
        } else {
            // Moving이 아닌 경우 속도 리셋
            vel.dir_x = 0.0f;
            vel.dir_z = 0.0f;
            vel.speed = 0.0f;
        }
    });
}
