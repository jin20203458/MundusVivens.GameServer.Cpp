#include "Systems.h"
#include "Components.h"
#include "TcpServer.h"
#include "GrpcResultQueue.h"
#include <iostream>
#include <algorithm>
#include <unordered_set>
#include <random>
#include <cmath>

// 1. 감정 쇠퇴 및 감정 전염 처리 시스템
void SystemEmotionDecay(entt::registry& reg) {
    if (!reg.ctx().contains<EmotionRegistry>()) return;
    const auto& emo_reg = reg.ctx().get<EmotionRegistry>();

    // 감정 전염 : 각 zone 별 부정적 감정 NPC가 있는지 먼저 스캔
    std::unordered_map<uint32_t, std::vector<uint8_t>> zone_negative_emotions; // zone_id -> 부정적 감정 ID 목록
    
    auto zone_view = reg.view<LocationComp, EmotionComp>();
    zone_view.each([&](LocationComp& loc, EmotionComp& emo) {
        if (emo.current_emotion_id < emo_reg.category_table.size()) {
            EmotionCategory cat = emo_reg.category_table[emo.current_emotion_id];
            if (cat != EmotionCategory::Neutral) {
                zone_negative_emotions[loc.zone_id].push_back(emo.current_emotion_id);
            }
        }
    });

    static std::random_device contagion_rd;
    static std::mt19937 contagion_gen(contagion_rd());
    static std::uniform_real_distribution<> contagion_dis(0.0, 1.0);

    // BusyTag가 부착된 NPC는 감정 쇠퇴/전염 연산에서 배제
    auto view = reg.view<EmotionComp, IdentityComp>(entt::exclude<BusyTag>);
    view.each([&](entt::entity entity, EmotionComp& emo, IdentityComp& identity) {
        // 감정 자연 쇠퇴
        if (emo.decay_ticks_remaining > 0) {
            emo.decay_ticks_remaining--;
            if (emo.decay_ticks_remaining == 0) {
                if (emo.current_emotion_id != emo.base_emotion_id) {
                    std::string old_emo = emo.current_emotion;
                    emo.current_emotion = emo.base_emotion;
                    emo.current_emotion_id = emo.base_emotion_id; // 정수 ID 동기화
                    std::cout << "🎭 [감정 쇠퇴] " << identity.display_name << "의 감정이 오래되어 기본 감정 [" 
                              << emo.base_emotion << "](으)로 자동 복귀되었습니다. (이전 감정: " << old_emo << ")" << std::endl;
                }
            }
        }

        // 감정 전염 적용 (불안/경계 전이)
        if (auto* loc_comp = reg.try_get<LocationComp>(entity)) {
            uint32_t zone_id = loc_comp->zone_id;
            auto it_neg = zone_negative_emotions.find(zone_id);
            if (it_neg != zone_negative_emotions.end() && !it_neg->second.empty()) {
                // "불안" 및 "경계" ID 최초 1회 캐싱
                static uint8_t anxiety_id = 255;
                static uint8_t alert_id = 255;
                static bool ids_cached = false;

                if (!ids_cached) {
                    auto& non_const_reg = const_cast<EmotionRegistry&>(emo_reg);
                    
                    auto it_anxiety = non_const_reg.name_to_id.find("불안");
                    if (it_anxiety == non_const_reg.name_to_id.end()) {
                        anxiety_id = static_cast<uint8_t>(non_const_reg.decay_ticks_table.size());
                        non_const_reg.name_to_id["불안"] = anxiety_id;
                        non_const_reg.decay_ticks_table.push_back(5);
                        non_const_reg.category_table.push_back(EmotionCategory::Neutral);
                    } else {
                        anxiety_id = it_anxiety->second;
                    }
                    
                    auto it_alert = non_const_reg.name_to_id.find("경계");
                    if (it_alert == non_const_reg.name_to_id.end()) {
                        alert_id = static_cast<uint8_t>(non_const_reg.decay_ticks_table.size());
                        non_const_reg.name_to_id["경계"] = alert_id;
                        non_const_reg.decay_ticks_table.push_back(5);
                        non_const_reg.category_table.push_back(EmotionCategory::Neutral);
                    } else {
                        alert_id = it_alert->second;
                    }
                    ids_cached = true;
                }

                // 이미 평온한 상태인 경우에만 감정 전염 (정수 ID 기반 판단)
                if (emo.current_emotion_id == emo.base_emotion_id && 
                    emo.current_emotion_id != anxiety_id && 
                    emo.current_emotion_id != alert_id) {
                    
                    // 마주친 부정 감정의 수 비례 확률 (개당 15%, 최대 45%)
                    float infect_chance = it_neg->second.size() * 0.15f;
                    if (infect_chance > 0.45f) infect_chance = 0.45f;

                    if (contagion_dis(contagion_gen) < infect_chance) {
                        uint8_t source_emo_id = it_neg->second[0];
                        EmotionCategory source_cat = EmotionCategory::Neutral;
                        if (source_emo_id < emo_reg.category_table.size()) {
                            source_cat = emo_reg.category_table[source_emo_id];
                        }

                        uint8_t target_emo_id = (source_cat == EmotionCategory::Hostility) ? alert_id : anxiety_id;
                        std::string target_emo = (source_cat == EmotionCategory::Hostility) ? "경계" : "불안";

                        emo.current_emotion = target_emo;
                        emo.current_emotion_id = target_emo_id;
                        emo.decay_ticks_remaining = 5; // 5틱 동안 지속 후 자동 쇠퇴

                        // 소스 감정 이름 조회 (로그용)
                        std::string source_emo_name = "부정적 감정";
                        for (const auto& pair : emo_reg.name_to_id) {
                            if (pair.second == source_emo_id) {
                                source_emo_name = pair.first;
                                break;
                            }
                        }

                        std::cout << "⚡ [감정 전염] " << identity.display_name << "이(가) 같은 구역 내 부정적 감정 [" 
                                  << source_emo_name << "]의 영향을 받아 [" << target_emo << "] 상태가 되었습니다!" << std::endl;
                    }
                }
            }
        }
    });
}

// 대화 트리거 & 다자간 합류 로직 시스템
void SystemSocialInteraction(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                             MundusVivens::AsyncGrpcClient& client, int tick,
                             std::mt19937& gen, std::uniform_real_distribution<>& dis,
                             GrpcResultQueue& grpc_queue) {
    
    // 매 틱마다 모든 NPC의 사회적 에너지를 미세하게 회복
    auto recovery_view = reg.view<CooldownComp, LocationComp>();
    recovery_view.each([tick](CooldownComp& cooldown, LocationComp& loc) {
        // Tavern/술집에 있으면 매 틱 1 회복. 일반 장소에서는 10틱에 1 회복
        bool in_tavern = (loc.type == LocationType::Tavern);
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
            if (!reg.all_of<ActivityComp>(ent)) continue; // 가구나 사물 엔티티 제외

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
        const auto& candidate_loc = reg.get<LocationComp>(candidates[0]);
        std::string zone_loc_name = candidate_loc.location_name;
        LocationType zone_loc_type = candidate_loc.type;
        float location_modifier = GetLocationSocialModifier(zone_loc_type);

        // 주도자(Initiator) 개별 판정
        for (entt::entity initiator : candidates) {
            if (reg.all_of<BusyTag>(initiator)) continue; // 이미 엮였을 수 있음

            auto& init_cooldown = reg.get_or_emplace<CooldownComp>(initiator);
            if (tick <= init_cooldown.last_initiative_tick) continue; // 스팸 방지

            float ext_i = 0.5f;
            if (auto* pers = reg.try_get<PersonalityComp>(initiator)) {
                ext_i = pers->extroversion;
            }

            // 주도 확률 계산
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
                if (auto* rel_comp = reg.try_get<RelationshipCacheComp>(initiator)) {
                    const auto& rels = rel_comp->relationships;
                    auto it_rel = rels.find(id_c);
                    if (it_rel != rels.end()) liking_i_to_c = it_rel->second.liking;
                }
                int liking_c_to_i = 0;
                if (auto* rel_comp = reg.try_get<RelationshipCacheComp>(target_candidate)) {
                    const auto& rels = rel_comp->relationships;
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
            float ext_t = 0.5f;
            if (auto* pers = reg.try_get<PersonalityComp>(target)) {
                ext_t = pers->extroversion;
            }
            int liking_t_to_i = 0;
            if (auto* rel_comp = reg.try_get<RelationshipCacheComp>(target)) {
                const auto& rels = rel_comp->relationships;
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
                    if (auto* rel_comp = reg.try_get<RelationshipCacheComp>(other)) {
                        const auto& rels = rel_comp->relationships;
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

                float ext_o = 0.5f;
                if (auto* pers = reg.try_get<PersonalityComp>(other)) {
                    ext_o = pers->extroversion;
                }
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
                if (group_participants.size() > 0) {
                    if (auto* ident = reg.try_get<IdentityComp>(group_participants[0])) {
                        start_payload.set_npc_a_name(ident->display_name);
                    }
                }
                if (group_participants.size() > 1) {
                    if (auto* ident = reg.try_get<IdentityComp>(group_participants[1])) {
                        start_payload.set_npc_b_name(ident->display_name);
                    }
                }
                start_payload.set_is_started(true);
                auto* start_loc = start_payload.mutable_location();
                start_loc->set_name(zone_loc_name);
                {
                    auto* start_pos = start_loc->mutable_position();
                    start_pos->set_x(loc_i.x);
                    start_pos->set_y(loc_i.y);
                    start_pos->set_z(loc_i.z);
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

                                // [브로드캐스트] 유니티 클라이언트에 대화 종료 및 대사 정보 전송
                                {
                                    mundusvivens::DialogueEventPayload end_payload;
                                    end_payload.set_task_id(result.task_id);
                                    if (group_participants.size() > 0) {
                                        if (auto* ident = reg.try_get<IdentityComp>(group_participants[0])) {
                                            end_payload.set_npc_a_name(ident->display_name);
                                        }
                                    }
                                    if (group_participants.size() > 1) {
                                        if (auto* ident = reg.try_get<IdentityComp>(group_participants[1])) {
                                            end_payload.set_npc_b_name(ident->display_name);
                                        }
                                    }
                                    end_payload.set_is_started(false);
                                    end_payload.set_summary(result.dialogue_summary);
                                    auto* end_loc = end_payload.mutable_location();
                                    end_loc->set_name(zone_loc_name);
                                    if (group_participants.size() > 0) {
                                        if (auto* end_pos_comp = reg.try_get<LocationComp>(group_participants[0])) {
                                            auto* end_pos = end_loc->mutable_position();
                                            end_pos->set_x(end_pos_comp->x);
                                            end_pos->set_y(end_pos_comp->y);
                                            end_pos->set_z(end_pos_comp->z);
                                        }
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

                                    if (reg.valid(target_ent)) {
                                        auto* emo_ptr = reg.try_get<EmotionComp>(target_ent);
                                        auto* identity_ptr = reg.try_get<IdentityComp>(target_ent);
                                        if (emo_ptr && identity_ptr) {
                                            auto& emo = *emo_ptr;
                                            const auto& identity = *identity_ptr;
                                        
                                            emo.current_emotion = em_update.new_emotion;

                                            int decay_ticks = 3;
                                            std::string intensity_str = "MEDIUM";

                                            if (em_update.intensity == 1) { // LOW
                                                decay_ticks = 3;
                                                intensity_str = "LOW";
                                            } else if (em_update.intensity == 3) { // HIGH
                                                decay_ticks = 10;
                                                intensity_str = "HIGH";
                                             } else { // MEDIUM
                                                 decay_ticks = 6;
                                                 intensity_str = "MEDIUM";
                                             }

                                            if (reg.ctx().contains<EmotionRegistry>()) {
                                                auto& emo_reg = reg.ctx().get<EmotionRegistry>();
                                                std::cout << "🎭 [감정 동기화] " << identity.display_name << "의 감정이 [" << em_update.new_emotion 
                                                          << "](으)로 업데이트되었습니다. 강도: [" << intensity_str << "] (" << decay_ticks << " 틱 유지)" << std::endl;
                                                auto it_emo = emo_reg.name_to_id.find(em_update.new_emotion);
                                                if (it_emo == emo_reg.name_to_id.end()) {
                                                    uint8_t next_id = static_cast<uint8_t>(emo_reg.decay_ticks_table.size());
                                                    emo_reg.name_to_id[em_update.new_emotion] = next_id;
                                                    emo_reg.decay_ticks_table.push_back(decay_ticks);
                                                    
                                                    // 딱 한 번만 카테고리 설정
                                                    EmotionCategory cat = EmotionCategory::Neutral;
                                                    if (em_update.category > 0) {
                                                        cat = static_cast<EmotionCategory>(em_update.category - 1);
                                                    }
                                                    emo_reg.category_table.push_back(cat);
                                                    
                                                    emo.current_emotion_id = next_id;
                                                } else {
                                                    emo.current_emotion_id = it_emo->second;
                                                }
                                            }
                                            emo.decay_ticks_remaining = decay_ticks;
                                        }
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
                                        job.category = static_cast<JobCategory>(nj.category);
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

                            // 엿듣기(Eavesdropping) 동작성 추가
                            if (!group_participants.empty() && reg.valid(group_participants[0])) {
                                uint32_t zone_id = reg.get<LocationComp>(group_participants[0]).zone_id;
                                if (zone_id != 0) {
                                    const auto& bystanders = grid.GetEntitiesInZone(zone_id);

                                    std::unordered_set<entt::entity> part_set(group_participants.begin(), group_participants.end());
                                    for (auto ent : bystanders) {
                                        if (part_set.find(ent) == part_set.end()) {
                                            if (reg.valid(ent) && reg.all_of<ActivityComp>(ent)) {
                                                if (auto* bystander_ident = reg.try_get<IdentityComp>(ent)) {
                                                    uint32_t bystander_id = bystander_ident->npc_id;
                                                    std::string bystander_name = bystander_ident->display_name;
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
                            }

                            // 행동 상태 갱신 및 상대별 쿨다운/사회적 에너지 소모 설정
                            for (auto ent : group_participants) {
                                if (reg.valid(ent)) {
                                    auto& cooldown = reg.get_or_emplace<CooldownComp>(ent);
                                    cooldown.social_energy = std::max(0, cooldown.social_energy - 30);
                                    cooldown.cognitive_refractory_until = tick + 3;
                                    reg.get<ActivityComp>(ent).current_activity = "생각 정리 중";
                                    
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

            break; // 한 zone에서 이번 틱에는 한 쌍만 대화 트리거
        }
    }
}

// 장소별 사회적 맥락 가중치 반환 헬퍼
float GetLocationSocialModifier(LocationType type) {
    switch (type) {
        case LocationType::Tavern:  return 1.8f;
        case LocationType::Market:  return 1.5f;
        case LocationType::Square:  return 1.2f;
        case LocationType::Church:  return 0.3f;
        default:                    return 1.0f;
    }
}
