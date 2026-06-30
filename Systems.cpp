#include "Systems.h"
#include "Components.h"
#include "TcpServer.h"
#include "ClientSession.h"
#include "mundus_vivens.pb.h"
#include <iostream>
#include <algorithm>
#include <unordered_set>

// 헬퍼 함수: NPC가 현재 진행 중인 활동(Activity)에 집중하여 대화를 피할 확률을 계산
bool IsNPCFocusedOnActivity(const std::string& activity, double roll) {
    if (activity.find("취침") != std::string::npos || activity.find("휴식") != std::string::npos) {
        // 취침/휴식 중에는 대화 불가
        return true;
    }
    if (activity.find("기도") != std::string::npos || activity.find("명상") != std::string::npos) {
        // 기도/명상 중에는 80% 확률로 대화 불가
        return roll < 0.8;
    }
    // 그 외 활동 시에는 30% 확률로 몰입하여 대화 거부
    return roll < 0.3;
}

// 바쁨 상태 동기화 시스템 (IsNpcBusy 대체)
void SystemUpdateBusyState(entt::registry& reg,
                           const std::unordered_map<std::string, PendingDialogue>& pendings,
                           const std::unordered_set<std::string>& busyAgentIdsFromCSharp) {
    // 1. 기존의 바쁨 상태 모두 초기화 (O(M_old))
    reg.clear<BusyTag>();

    // 2. 비동기 대화 중인 NPC 바쁨 상태 부여
    for (const auto& [task_id, pd] : pendings) {
        if (reg.valid(pd.npc_a) && !reg.all_of<BusyTag>(pd.npc_a)) reg.emplace<BusyTag>(pd.npc_a);
        if (reg.valid(pd.npc_b) && !reg.all_of<BusyTag>(pd.npc_b)) reg.emplace<BusyTag>(pd.npc_b);
    }

    // 3. 플레이어 대화 중인 NPC & 플레이어 바쁨 상태 부여
    auto player_dialogue_view = reg.view<PlayerDialogueComp>();
    player_dialogue_view.each([&](entt::entity player_ent, const PlayerDialogueComp& pdc) {
        if (!reg.all_of<BusyTag>(player_ent)) reg.emplace<BusyTag>(player_ent);
        if (reg.valid(pdc.npc_entity) && !reg.all_of<BusyTag>(pdc.npc_entity)) {
            reg.emplace<BusyTag>(pdc.npc_entity);
        }
    });

    // 4. C#에서 직접 바쁨 상태로 지정한 NPC 바쁨 상태 부여
    if (reg.ctx().contains<EntityIndex>()) {
        const auto& entity_index = reg.ctx().get<EntityIndex>();
        for (const auto& npc_id : busyAgentIdsFromCSharp) {
            auto it = entity_index.by_npc_id.find(npc_id);
            if (it != entity_index.by_npc_id.end()) {
                if (!reg.all_of<BusyTag>(it->second)) {
                    reg.emplace<BusyTag>(it->second);
                }
            }
        }
    }
}

// 1. 감정 쇠퇴 처리 시스템
void SystemEmotionDecay(entt::registry& reg) {
    // BusyTag가 부착된 NPC는 감정 쇠퇴 연산에서 아예 배제 (O(1) 필터링)
    auto view = reg.view<EmotionComp, IdentityComp>(entt::exclude<BusyTag>);
    view.each([&](EmotionComp& emo, IdentityComp& identity) {
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
    });
}

// 2. 스케줄 기반 NPC 위치 이동 및 SpatialGrid 업데이트 시스템
void SystemScheduleMovement(entt::registry& reg, SpatialHashGrid& grid, int current_tick) {
    int current_hour = current_tick % 24;
    auto view = reg.view<LocationComp, ActivityComp, ScheduleComp, IdentityComp>();

    view.each([&](entt::entity entity, LocationComp& loc, ActivityComp& act, ScheduleComp& sched, IdentityComp& identity) {
        // 대화 중이거나 바쁜 NPC는 이동 제한 (BusyTag 검사 O(1))
        // TODO: 만약 하단의 디버그 로그가 불필요해진다면, 뷰 생성 단계에서 entt::exclude<BusyTag>를 사용하여 
        // 람다 함수 진입 오버헤드 자체를 생략하는 것이 성능상 더욱 좋습니다.
        if (reg.all_of<BusyTag>(entity)) {
            std::cout << "💬 [이동 제한] " << identity.display_name << "은(는) 대화 중이므로 이동할 수 없습니다. 위치 유지: [" 
                      << loc.location_name << "]" << std::endl;
            return;
        }

        std::string target_location = loc.location_name;
        std::string scheduled_activity = act.current_activity;
        bool found_schedule = false;

        for (const auto& item : sched.items) {
            if (item.start_hour <= current_hour && current_hour <= item.end_hour) {
                target_location = item.target_location;
                scheduled_activity = item.activity;
                found_schedule = true;
                break;
            }
        }

        std::string old_loc = loc.location_name;
        if (found_schedule) {
            loc.location_name = target_location;
            act.current_activity = scheduled_activity;

            uint32_t old_zone = loc.zone_id;
            uint32_t new_zone = grid.GetOrCreateZoneId(target_location);
            loc.zone_id = new_zone;

            if (new_zone != old_zone) {
                grid.Move(entity, old_zone, new_zone);
                std::cout << "🏃 [스케줄 이동] " << identity.display_name << " 이동함: [" << old_loc << "] ➔ [" 
                          << target_location << "] (행동: " << scheduled_activity << ")" << std::endl;
            } else {
                grid.Insert(entity, new_zone);
                std::cout << "🧍 [스케줄 유지] " << identity.display_name << " 위치 유지: [" << target_location 
                          << "] (행동: " << scheduled_activity << ")" << std::endl;
            }
        } else {
            uint32_t current_zone = grid.GetOrCreateZoneId(loc.location_name);
            loc.zone_id = current_zone;
            grid.Insert(entity, current_zone);
            std::cout << "🧍 [위치 잔류] " << identity.display_name << " 위치 유지: [" << loc.location_name 
                      << "] (현재 행동: " << act.current_activity << ")" << std::endl;
        }
    });
}

// 3. 비동기 대화 결과 수거 시스템
void SystemPollDialogueResults(entt::registry& reg, SpatialHashGrid& grid,
                               MundusVivens::AsyncGrpcClient& client, int tick,
                               std::unordered_map<std::string, PendingDialogue>& pendingDialogues) {

    for (auto it = pendingDialogues.begin(); it != pendingDialogues.end(); ) {
        const auto& pd = it->second;
        // [기아 방지] 10틱(50초) 이상 완료 응답이 없으면 타임아웃 강제 해제
        if (tick - pd.triggered_tick > 10) {
            std::string name_a = reg.valid(pd.npc_a) ? reg.get<IdentityComp>(pd.npc_a).display_name : "Unknown";
            std::string name_b = reg.valid(pd.npc_b) ? reg.get<IdentityComp>(pd.npc_b).display_name : "Unknown";

            std::cerr << "⚠️ [대화 타임아웃] Job " << pd.task_id
                      << " (" << name_a << " <-> " << name_b
                      << ", 장소: " << pd.meeting_location
                      << ") 응답 없음 — 강제 해제합니다." << std::endl;

            if (reg.valid(pd.npc_a)) {
                reg.get<ActivityComp>(pd.npc_a).current_activity = "대기";
                auto& cooldown = reg.get_or_emplace<CooldownComp>(pd.npc_a);
                cooldown.global_cooldown_until = tick + 3;
            }
            if (reg.valid(pd.npc_b)) {
                reg.get<ActivityComp>(pd.npc_b).current_activity = "대기";
                auto& cooldown = reg.get_or_emplace<CooldownComp>(pd.npc_b);
                cooldown.global_cooldown_until = tick + 3;
            }

            // BusyTag 해제
            if (reg.valid(pd.npc_a) && reg.all_of<BusyTag>(pd.npc_a)) reg.erase<BusyTag>(pd.npc_a);
            if (reg.valid(pd.npc_b) && reg.all_of<BusyTag>(pd.npc_b)) reg.erase<BusyTag>(pd.npc_b);

            it = pendingDialogues.erase(it);
            continue;
        }

        if (!it->second.poll_requested) {
            it->second.poll_requested = true;
            std::string task_id = it->first;
            
            client.PollDialogueResultAsync(task_id, [&reg, &pendingDialogues, task_id, tick](bool success, const MundusVivens::DialogueResult& result) {
                auto pd_it = pendingDialogues.find(task_id);
                if (pd_it == pendingDialogues.end()) return; // 이미 타임아웃 등으로 삭제됨

                if (!success) {
                    pd_it->second.poll_requested = false;
                    std::cerr << "⏳ [대화 결과 수거 에러] Job " << task_id << " 통신 에러 발생. 다음 틱에 재시도합니다." << std::endl;
                    return;
                }

                if (result.is_completed) {
                    const auto& pd_val = pd_it->second;
                    std::string name_a = reg.valid(pd_val.npc_a) ? reg.get<IdentityComp>(pd_val.npc_a).display_name : "Unknown";
                    std::string name_b = reg.valid(pd_val.npc_b) ? reg.get<IdentityComp>(pd_val.npc_b).display_name : "Unknown";

                    std::cout << "\n🔔 [비동기 대화 완료 수신] [" << pd_val.meeting_location << "]에서 진행된 "
                              << name_a << "와(과) " << name_b << "의 대화 완료!" << std::endl;

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

                    // C++ 내부 감정 상태 갱신 (EntityIndex O(1) 역방향 인덱스 활용)
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

                    // 행동 상태 갱신 및 쿨다운 설정
                    if (reg.valid(pd_val.npc_a)) {
                        reg.get<ActivityComp>(pd_val.npc_a).current_activity = "대화 마침";
                        auto& cooldown = reg.get_or_emplace<CooldownComp>(pd_val.npc_a);
                        cooldown.global_cooldown_until = tick + 3;
                        if (reg.all_of<BusyTag>(pd_val.npc_a)) reg.erase<BusyTag>(pd_val.npc_a);
                    }
                    if (reg.valid(pd_val.npc_b)) {
                        reg.get<ActivityComp>(pd_val.npc_b).current_activity = "대화 마침";
                        auto& cooldown = reg.get_or_emplace<CooldownComp>(pd_val.npc_b);
                        cooldown.global_cooldown_until = tick + 3;
                        if (reg.all_of<BusyTag>(pd_val.npc_b)) reg.erase<BusyTag>(pd_val.npc_b);
                    }

                    pendingDialogues.erase(pd_it);
                } else {
                    pd_it->second.poll_requested = false;
                    std::string name_a = reg.valid(pd_it->second.npc_a) ? reg.get<IdentityComp>(pd_it->second.npc_a).display_name : "Unknown";
                    std::string name_b = reg.valid(pd_it->second.npc_b) ? reg.get<IdentityComp>(pd_it->second.npc_b).display_name : "Unknown";
                    std::cout << "⏳ [대화 진행 중] Job " << task_id << " (" << name_a << " <-> " << name_b 
                              << ") 연산 대기 중..." << std::endl;
                }
            });
        }
        ++it;
    }
}

// 4. 공간 그리드 기반 동일 구역 내 NPC 간 대화 트리거 시스템
void SystemSpatialDialogueTrigger(entt::registry& reg, SpatialHashGrid& grid,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::unordered_map<std::string, PendingDialogue>& pendingDialogues,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis) {
    // 공간 해시 그리드의 각 zone을 순회
    for (const auto& [zone_id, entities] : grid.AllZones()) {
        if (entities.size() < 2) continue; // 혼자 있는 구역은 건너뜀

        // 구역 내 NPC 순서를 무작위로 섞어서 앞 순서 독식 편향 제거
        std::vector<entt::entity> shuffled_entities = entities;
        std::shuffle(shuffled_entities.begin(), shuffled_entities.end(), gen);

        for (size_t i = 0; i < shuffled_entities.size(); ++i) {
            entt::entity ent_a = shuffled_entities[i];
            if (!reg.valid(ent_a)) continue;

            // 플레이어는 자동 대화 트리거 검사에서 제외 (ActivityComp 누락으로 인한 크래시 방지)
            if (reg.all_of<PlayerTag>(ent_a)) continue;

            // BusyTag O(1) 확인
            if (reg.all_of<BusyTag>(ent_a)) continue;

            const auto& cooldown_a = reg.get_or_emplace<CooldownComp>(ent_a);
            if (tick < cooldown_a.global_cooldown_until) continue;
            if (cooldown_a.daily_dialogue_count >= 3) continue; // 일일 대화 제한

            const auto& act_a = reg.get<ActivityComp>(ent_a);
            const auto& identity_a = reg.get<IdentityComp>(ent_a);

            // 60% 대화 발동 확률 상한선 게이트 (대화 확률이 인원 수에 비례하되 최대 60%로 수렴하도록 제어)
            if (dis(gen) >= 0.6) {
                continue; // 40% 확률로 이번 틱에는 대화를 시도하지 않음
            }

            for (size_t j = i + 1; j < shuffled_entities.size(); ++j) {
                entt::entity ent_b = shuffled_entities[j];
                if (!reg.valid(ent_b)) continue;

                // 플레이어는 자동 대화 트리거 검사에서 제외
                if (reg.all_of<PlayerTag>(ent_b)) continue;

                if (reg.all_of<BusyTag>(ent_b)) continue;

                const auto& cooldown_b = reg.get_or_emplace<CooldownComp>(ent_b);
                if (tick < cooldown_b.global_cooldown_until) continue;
                if (cooldown_b.daily_dialogue_count >= 3) continue; // 일일 대화 제한

                const auto& act_b = reg.get<ActivityComp>(ent_b);
                const auto& identity_b = reg.get<IdentityComp>(ent_b);

                // 현재 활동에 몰입하고 있어 대화를 거부하는지 체크
                double rollA = dis(gen);
                double rollB = dis(gen);
                if (IsNPCFocusedOnActivity(act_a.current_activity, rollA)) {
                    std::cout << "⏳ [대화 억제] " << identity_a.display_name << "은(는) 현재 활동(\"" 
                              << act_a.current_activity << "\")에 몰입해 대화를 나누지 못합니다." << std::endl;
                    continue;
                }
                if (IsNPCFocusedOnActivity(act_b.current_activity, rollB)) {
                    std::cout << "⏳ [대화 억제] " << identity_b.display_name << "은(는) 현재 활동(\"" 
                              << act_b.current_activity << "\")에 몰입해 대화를 나누지 못합니다." << std::endl;
                    continue;
                }

                // 50%의 대화 발동 주사위 확률 검사
                if (dis(gen) < 0.5) {
                    const auto& loc_a = reg.get<LocationComp>(ent_a);
                    std::string loc_name = loc_a.location_name;
                    std::string id_a = identity_a.npc_id;
                    std::string id_b = identity_b.npc_id;
                    std::string name_a = identity_a.display_name;
                    std::string name_b = identity_b.display_name;

                    std::cout << "\n💬 [C++ 공간 충돌 감지] " << name_a << "와(과) " << name_b 
                              << "이(가) [" << loc_name << "] 공간에서 마주쳤습니다!" << std::endl;
                    std::cout << "💬 비동기 gRPC 통신으로 대화 트리거를 요청합니다 (완전 비동기)..." << std::endl;

                    // 중복 대화 요청 방지를 위해 즉시 임시 Busy 상태 적용
                    reg.get<ActivityComp>(ent_a).current_activity = name_b + "와(과) 대화 요청 중";
                    reg.get<ActivityComp>(ent_b).current_activity = name_a + "와(과) 대화 요청 중";
                    reg.emplace_or_replace<BusyTag>(ent_a);
                    reg.emplace_or_replace<BusyTag>(ent_b);

                    // 비동기 트리거 호출
                    client.TriggerDialogueAsync(id_a, id_b, [&reg, &pendingDialogues, ent_a, ent_b, tick, loc_name, name_a, name_b](bool success, const MundusVivens::DialogueResult& result) {
                        if (success && result.is_queued) {
                            std::cout << "🚀 대화가 대기열에 성공적으로 등록되었습니다. Job ID: " << result.task_id << std::endl;
                            if (reg.valid(ent_a) && reg.valid(ent_b)) {
                                pendingDialogues[result.task_id] = { result.task_id, ent_a, ent_b, tick, loc_name, false };
                                reg.get<CooldownComp>(ent_a).daily_dialogue_count++;
                                reg.get<CooldownComp>(ent_b).daily_dialogue_count++;

                                reg.get<ActivityComp>(ent_a).current_activity = name_b + "와(과) 대화 중";
                                reg.get<ActivityComp>(ent_b).current_activity = name_a + "와(과) 대화 중";

                                reg.emplace_or_replace<BusyTag>(ent_a);
                                reg.emplace_or_replace<BusyTag>(ent_b);
                            }
                        } else {
                            std::cerr << "❌ [대화 요청 실패] 대화가 큐에 등록되지 못했습니다." << std::endl;
                            if (reg.valid(ent_a)) {
                                reg.get<ActivityComp>(ent_a).current_activity = "대기";
                                if (reg.all_of<BusyTag>(ent_a)) reg.erase<BusyTag>(ent_a);
                            }
                            if (reg.valid(ent_b)) {
                                reg.get<ActivityComp>(ent_b).current_activity = "대기";
                                if (reg.all_of<BusyTag>(ent_b)) reg.erase<BusyTag>(ent_b);
                            }
                        }
                    });

                    break; // 한 틱에 한 명은 한 번만 대화 시작 가능하도록 내부 루프 나감
                }
            }
        }
    }
}



// 6. 변경 상태 배치 gRPC 동기화 시스템
void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client) {
    std::vector<MundusVivens::AgentStatusUpdate> updates;
    std::vector<entt::entity> target_entities;
    auto view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp, LastSyncedComp>();

    view.each([&](entt::entity entity, IdentityComp& identity, LocationComp& loc, EmotionComp& emo, ActivityComp& act, LastSyncedComp& sync) {
        if (sync.location != loc.location_name || sync.emotion != emo.current_emotion || sync.activity != act.current_activity) {
            MundusVivens::AgentStatusUpdate update;
            update.agent_id = identity.npc_id;
            update.location = loc.location_name;
            update.emotion = emo.current_emotion;
            update.activity = act.current_activity;
            updates.push_back(update);
            target_entities.push_back(entity);
        }
    });

    if (!updates.empty()) {
        // 비동기 콜백 캡처 시 std::move 적용(D-5) 및 target_entities 병렬 캡처 적용
        client.BatchUpdateStatusAsync(updates, [&reg, target_entities = std::move(target_entities), updates = std::move(updates)](bool success, int32_t updated_count, const std::string& message) {
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
    }
}

// 7. 연결 끊긴 플레이어의 대화 정리 시스템 구현
void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
    std::vector<entt::entity> to_cleanup;
    auto view = reg.view<PlayerTag, PlayerDialogueComp>();
    view.each([&](entt::entity entity, const PlayerTag& tag, const PlayerDialogueComp&) {
        if (tcp.GetSession(tag.session_index) == nullptr) {
            to_cleanup.push_back(entity);
        }
    });

    for (auto player_ent : to_cleanup) {
        const auto& pdc = reg.get<PlayerDialogueComp>(player_ent);
        
        std::cout << "⚠️ [네트워크 끊김 감지] 플레이어 세션(" << reg.get<PlayerTag>(player_ent).session_index 
                  << ") 연결 끊김 확인. 강제로 대화(Session: " << pdc.session_id 
                  << ")를 종료하고 NPC를 해방합니다." << std::endl;

        // 1. C# AI 서버에 종료 신호 전송 (비동기)
        async_client.EndPlayerDialogueAsync(pdc.session_id, [](bool success, const std::string& summary) {
            std::cout << "💬 [비정상 종료 대화 정리 완료] C# AI 서버 대화 종료 응답 수신: " << summary << std::endl;
        });

        // 2. NPC 상태 정상 복귀
        if (reg.valid(pdc.npc_entity)) {
            if (reg.all_of<ActivityComp>(pdc.npc_entity)) {
                reg.get<ActivityComp>(pdc.npc_entity).current_activity = "대기";
            }
            if (reg.all_of<BusyTag>(pdc.npc_entity)) {
                reg.erase<BusyTag>(pdc.npc_entity);
            }
        }

        // 3. 플레이어 본인의 BusyTag 및 대화 컴포넌트 제거
        if (reg.all_of<BusyTag>(player_ent)) {
            reg.erase<BusyTag>(player_ent);
        }
        reg.erase<PlayerDialogueComp>(player_ent);
    }
}

// 8. 플레이어 커맨드 처리 시스템 구현
void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick) {
    std::vector<PlayerCommand> commands = tcp.DrainPlayerCommands();
    auto& index = reg.ctx().get<EntityIndex>();

    for (const auto& cmd : commands) {
        if (cmd.type == PlayerCommand::Login) {
            mundusvivens::LoginRequest req;
            if (!req.ParseFromString(cmd.payload)) continue;

            // 플레이어 검색 혹은 신규 생성 (EntityIndex O(1) 활용)
            entt::entity player_ent = entt::null;
            auto it = index.by_npc_id.find(cmd.player_id);
            if (it != index.by_npc_id.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) {
                player_ent = reg.create();
                reg.emplace<PlayerTag>(player_ent, cmd.session_index);
                reg.emplace<IdentityComp>(player_ent, cmd.player_id, req.player_name());
                reg.emplace<LocationComp>(player_ent, 0u, "광장");
                reg.emplace<LastSyncedComp>(player_ent);

                index.by_npc_id[cmd.player_id] = player_ent;
                index.by_session_index[cmd.session_index] = player_ent;

                std::cout << "👤 [플레이어 로그인] 신규 플레이어 등록: " << req.player_name() << " (ID: " << cmd.player_id << ")" << std::endl;
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

            resp.add_locations("광장");
            resp.add_locations("여관");
            resp.add_locations("시장");
            resp.add_locations("성당");

            // 현재 NPC 전체 상태 추가
            auto npc_view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp>(entt::exclude<PlayerTag>);
            npc_view.each([&](const IdentityComp& npc_id, const LocationComp& npc_loc, const EmotionComp& npc_emo, const ActivityComp& npc_act) {
                auto* snapshot = resp.add_npcs();
                snapshot->set_npc_id(npc_id.npc_id);
                snapshot->set_display_name(npc_id.display_name);
                snapshot->set_location(npc_loc.location_name);
                snapshot->set_emotion(npc_emo.current_emotion);
                snapshot->set_activity(npc_act.current_activity);
            });

            std::string serialized;
            if (resp.SerializeToString(&serialized)) {
                tcp.SendTo(cmd.session_index, PacketId::SC_LOGIN_ACK, serialized);
            }
        }
        else if (cmd.type == PlayerCommand::Move) {
            mundusvivens::PlayerMoveRequest req;
            if (!req.ParseFromString(cmd.payload)) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent != entt::null) {
                auto& loc = reg.get<LocationComp>(player_ent);
                const auto& identity = reg.get<IdentityComp>(player_ent);
                std::string old_loc = loc.location_name;
                std::string new_loc = req.target_location();

                uint32_t old_zone = loc.zone_id;
                uint32_t new_zone = grid.GetOrCreateZoneId(new_loc);

                loc.location_name = new_loc;
                loc.zone_id = new_zone;
                grid.Move(player_ent, old_zone, new_zone);

                std::cout << "🏃 [TCP 플레이어 이동] 플레이어 " << identity.display_name 
                          << " 이동: [" << old_loc << "] ➔ [" << new_loc << "]" << std::endl;
            }
        }
        else if (cmd.type == PlayerCommand::TalkToNpc) {
            mundusvivens::TalkToNpcRequest req;
            if (!req.ParseFromString(cmd.payload)) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) continue;
            const auto& player_loc = reg.get<LocationComp>(player_ent);

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

            async_client.StartPlayerDialogueAsync(cmd.player_id, req.npc_id(), 
                [&reg, tcp_addr = &tcp, session_idx, npc_name, npc_ent, player_ent, session](bool success, const std::string& session_id, const std::string& greeting, const std::string& message) {
                    if (session) session->DecrementPendingGrpc();
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

                        std::string serialized;
                        if (reply.SerializeToString(&serialized)) {
                            tcp_addr->SendTo(session_idx, PacketId::SC_NPC_REPLY, serialized);
                        }
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
                }
            );
        }
        else if (cmd.type == PlayerCommand::PlayerMessage) {
            mundusvivens::PlayerMessageRequest req;
            if (!req.ParseFromString(cmd.payload)) continue;

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
                [tcp_addr = &tcp, session_idx, npc_name, req, session](bool success, const std::string& reply_text) {
                    if (session) session->DecrementPendingGrpc();
                    if (success) {
                        mundusvivens::NpcReplyPayload reply;
                        reply.set_session_id(req.session_id());
                        reply.set_npc_name(npc_name);
                        reply.set_reply_text(reply_text);

                        std::string serialized;
                        if (reply.SerializeToString(&serialized)) {
                            tcp_addr->SendTo(session_idx, PacketId::SC_NPC_REPLY, serialized);
                        }
                    } else {
                        std::cerr << "❌ [플레이어 메시지 전송 실패] 대화 응답 전송 실패" << std::endl;
                    }
                }
            );
        }
        else if (cmd.type == PlayerCommand::EndDialogue) {
            mundusvivens::EndDialogueRequest req;
            if (!req.ParseFromString(cmd.payload)) continue;

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
                [&reg, npc_ent, player_ent, session](bool success, const std::string& summary) {
                    if (session) session->DecrementPendingGrpc();
                    std::cout << "💬 [플레이어 대화 종료 수신] 요약: " << summary << std::endl;
                    if (reg.valid(npc_ent)) {
                        reg.get<ActivityComp>(npc_ent).current_activity = "대기";
                        if (reg.all_of<BusyTag>(npc_ent)) reg.erase<BusyTag>(npc_ent);
                    }
                    if (reg.valid(player_ent)) {
                        if (reg.all_of<BusyTag>(player_ent)) reg.erase<BusyTag>(player_ent);
                        if (reg.all_of<PlayerDialogueComp>(player_ent)) reg.erase<PlayerDialogueComp>(player_ent);
                    }
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
        snapshot->set_location(loc.location_name);
        snapshot->set_emotion(emo.current_emotion);
        snapshot->set_activity(act.current_activity);
    });

    std::string serialized;
    if (payload.SerializeToString(&serialized)) {
        tcp.BroadcastPacket(PacketId::SC_WORLD_SNAPSHOT, serialized);
    }
}
