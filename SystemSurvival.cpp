#include "Systems.h"
#include "Components.h"
#include "GrpcResultQueue.h"
#include <iostream>
#include <cmath>

// 🆕 생체 욕구 및 인터럽트 오버라이드 시스템
void SystemSurvivalOverride(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    auto view = reg.view<NeedsComp, LocationComp, ToilComp, JobComp, IdentityComp>();
    view.each([&](entt::entity entity, NeedsComp& needs, LocationComp& loc, ToilComp& toil, JobComp& job, IdentityComp& identity) {
        auto& path = reg.get_or_emplace<PathfindingComp>(entity);

        // 계획 수면/식사 여부 판별
        bool is_scheduled_sleep = false;
        bool is_scheduled_eat = false;
        if (job.is_active && toil.state == ToilState::Working) {
            std::string intent = job.intent;
            if (intent.find("Sleep") != std::string::npos || intent.find("sleep") != std::string::npos ||
                intent.find("잠") != std::string::npos || intent.find("수면") != std::string::npos ||
                intent.find("취침") != std::string::npos || intent.find("휴식") != std::string::npos || 
                intent.find("rest") != std::string::npos) {
                is_scheduled_sleep = true;
            } else if (intent.find("Eat") != std::string::npos || intent.find("eat") != std::string::npos ||
                       intent.find("밥") != std::string::npos || intent.find("식사") != std::string::npos ||
                       intent.find("음식") != std::string::npos || intent.find("먹기") != std::string::npos ||
                       intent.find("취식") != std::string::npos) {
                is_scheduled_eat = true;
            }
        }

        // 매 물리 틱마다 욕구 감소 연산 (20Hz 기준 서서히 감쇠)
        // 대뇌 락 중이거나 계획 활동을 통해 해결 중인 생체 욕구만 감쇠를 면제함
        bool skip_hunger_decay = (needs.is_resolving_survival && needs.current_survival_type == "hunger") || (is_scheduled_eat && needs.occupied_furniture != entt::null);
        bool skip_fatigue_decay = (needs.is_resolving_survival && needs.current_survival_type == "fatigue") || (is_scheduled_sleep && needs.occupied_furniture != entt::null);

        if (!skip_hunger_decay) {
            needs.hunger -= 0.045f;
        }
        if (!skip_fatigue_decay) {
            needs.fatigue -= 0.025f;
        }

        if (needs.hunger < 0.0f) needs.hunger = 0.0f;
        if (needs.fatigue < 0.0f) needs.fatigue = 0.0f;

        // 아직 로컬 생존 로직이 구동 중이지 않은 평시 상태일 때
        if (!needs.is_resolving_survival) {
            bool trigger_hunger = (needs.hunger < 15.0f);
            bool trigger_fatigue = (needs.fatigue < 15.0f);

            if (trigger_hunger || trigger_fatigue) {
                needs.is_resolving_survival = true;

                // 생체 위기 시 Busy 상태(대화, 대기 등) 강제 해제하여 이동 보장
                if (reg.all_of<BusyTag>(entity)) {
                    reg.erase<BusyTag>(entity);
                }

                std::string survival_type = trigger_hunger ? "hunger" : "fatigue";
                needs.current_survival_type = survival_type;

                std::cout << "🚨 [생체 위기 발생] " << identity.display_name << "의 생물학적 욕구 위험 상태 돌입! (허기: " 
                          << needs.hunger << "/100, 피로: " << needs.fatigue << "/100)" << std::endl;

                // 기존 진행 중이던 고차원 C# Job 강제 차단 보고
                uint32_t npc_id = identity.npc_id;
                uint64_t job_id = job.job_id;
                std::string context_str = "survival_" + survival_type;

                // C# 대뇌에 인터럽트 통보 (survival 키워드로 성찰 API 우회 트리거)
                client.ReportJobStatusAsync(npc_id, job_id, 2, mundusvivens::ENVIRONMENT_CHANGE, context_str, current_tick,
                    MundusVivens::AsyncGrpcClient::ReportJobStatusCallback([&grpc_queue, entity](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                        grpc_queue.Push([success](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                            if (success) {
                                std::cout << "📥 [생체 중단 보고 완료] C# 서버 수신 및 우회 확인." << std::endl;
                            }
                        });
                    }));

                // C++ 척수 수준에서 이동 중인 경로 초기화
                path.waypoints.clear();
                path.current_waypoint_index = 0;

                // 로컬 생존을 위한 임시 Job 주입 (동적 가구/사물 타겟 검색으로 하드코딩 완전 제거)
                if (survival_type == "hunger") {
                    job.job_id = 999000; // 로컬 기아 해결 가상 Job ID
                    job.intent = "식사 중";

                    entt::entity target_furn = entt::null;
                    float min_dist = 999999.0f;
                    std::string found_loc = "";
                    float found_x = 30.0f, found_y = 0.0f, found_z = 40.0f;

                    auto furn_view = reg.view<AffordanceComp, LocationComp>();
                    furn_view.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc) {
                        if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                            float dx = furn_loc.x - loc.x;
                            float dz = furn_loc.z - loc.z;
                            float dist = std::sqrt(dx * dx + dz * dz);
                            if (furn_loc.zone_id != loc.zone_id) {
                                dist += 1000.0f;
                            }
                            if (dist < min_dist) {
                                min_dist = dist;
                                target_furn = furn_ent;
                                found_loc = furn_loc.location_name;
                                found_x = furn_loc.x;
                                found_y = furn_loc.y;
                                found_z = furn_loc.z;
                            }
                        }
                    });

                    if (target_furn != entt::null && min_dist < 50.0f) {
                        job.target_location = found_loc;
                        job.target_x = found_x;
                        job.target_y = found_y;
                        job.target_z = found_z;
                    } else {
                        // 황무지 야영: 임시 모닥불 엔티티 동적 생성
                        entt::entity camp_ent = reg.create();
                        reg.emplace<IdentityComp>(camp_ent, 0u, "임시 모닥불");
                        reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                        reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Eat, entt::null);
                        
                        grid.Insert(camp_ent, loc.zone_id);
                        
                        target_furn = camp_ent;
                        job.target_location = loc.location_name;
                        job.target_x = loc.x;
                        job.target_y = loc.y;
                        job.target_z = loc.z;
                        std::cout << "🔥 [야영 개시] " << identity.display_name << "이(가) 황무지 현위치에 [" 
                                  << reg.get<IdentityComp>(camp_ent).display_name << "]을 피우고 식사 준비를 합니다." << std::endl;
                    }
                } else {
                    job.job_id = 999001; // 로컬 피로 해결 가상 Job ID
                    job.intent = "취침 중";

                    entt::entity target_furn = entt::null;
                    float min_dist = 999999.0f;
                    std::string found_loc = "";
                    float found_x = 0.0f, found_y = 0.0f, found_z = 0.0f;

                    auto furn_view = reg.view<AffordanceComp, LocationComp>();
                    furn_view.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc) {
                        if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                            float dx = furn_loc.x - loc.x;
                            float dz = furn_loc.z - loc.z;
                            float dist = std::sqrt(dx * dx + dz * dz);
                            if (furn_loc.zone_id != loc.zone_id) {
                                dist += 1000.0f;
                            }
                            if (dist < min_dist) {
                                min_dist = dist;
                                target_furn = furn_ent;
                                found_loc = furn_loc.location_name;
                                found_x = furn_loc.x;
                                found_y = furn_loc.y;
                                found_z = furn_loc.z;
                            }
                        }
                    });

                    if (target_furn != entt::null && min_dist < 50.0f) {
                        job.target_location = found_loc;
                        job.target_x = found_x;
                        job.target_y = found_y;
                        job.target_z = found_z;
                    } else {
                        // 황무지 야영: 임시 야영지 엔티티 동적 생성
                        entt::entity camp_ent = reg.create();
                        reg.emplace<IdentityComp>(camp_ent, 0u, "임시 야영지");
                        reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                        reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Sleep, entt::null);
                        
                        grid.Insert(camp_ent, loc.zone_id);
                        
                        target_furn = camp_ent;
                        job.target_location = loc.location_name;
                        job.target_x = loc.x;
                        job.target_y = loc.y;
                        job.target_z = loc.z;
                        std::cout << "⛺ [야영 개시] " << identity.display_name << "이(가) 황무지 현위치에 [" 
                                  << reg.get<IdentityComp>(camp_ent).display_name << "]를 구축하고 잠을 청합니다." << std::endl;
                    }
                }
                job.is_active = true;
                toil.state = ToilState::Idle;
            }
        }
    });
}

// 🆕 사물 상호작용 및 욕구 충전 시스템
void SystemAffordanceResolver(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    // 1. 점유 가구 상태 예외 보정
    auto furn_view = reg.view<AffordanceComp>();
    furn_view.each([&](entt::entity furn_ent, AffordanceComp& aff) {
        if (aff.occupied_by != entt::null) {
            if (!reg.valid(aff.occupied_by)) {
                aff.occupied_by = entt::null;
            } else {
                auto* needs = reg.try_get<NeedsComp>(aff.occupied_by);
                if (!needs || needs->occupied_furniture != furn_ent) {
                    aff.occupied_by = entt::null;
                }
            }
        }
    });

    // 2. 생체 욕구를 해결하고 있는 NPC들의 상태 조율
    auto npc_view = reg.view<NeedsComp, LocationComp, ToilComp, JobComp, IdentityComp>();
    npc_view.each([&](entt::entity npc_entity, NeedsComp& needs, LocationComp& loc, ToilComp& toil, JobComp& job, IdentityComp& identity) {
        // 계획 수면/식사 여부 판별
        bool is_scheduled_sleep = false;
        bool is_scheduled_eat = false;
        if (job.is_active && toil.state == ToilState::Working) {
            std::string intent = job.intent;
            if (intent.find("Sleep") != std::string::npos || intent.find("sleep") != std::string::npos ||
                intent.find("잠") != std::string::npos || intent.find("수면") != std::string::npos ||
                intent.find("취침") != std::string::npos || intent.find("휴식") != std::string::npos || 
                intent.find("rest") != std::string::npos) {
                is_scheduled_sleep = true;
            } else if (intent.find("Eat") != std::string::npos || intent.find("eat") != std::string::npos ||
                       intent.find("밥") != std::string::npos || intent.find("식사") != std::string::npos ||
                       intent.find("음식") != std::string::npos || intent.find("먹기") != std::string::npos ||
                       intent.find("취식") != std::string::npos) {
                is_scheduled_eat = true;
            }
        }

        // 일반 계획 완료/취소/교체 시 점유 중인 가구 자동 반납 및 철거
        if (!needs.is_resolving_survival && needs.occupied_furniture != entt::null) {
            bool keep_furniture = false;
            if (job.is_active && toil.state == ToilState::Working) {
                if (is_scheduled_sleep) {
                    if (reg.valid(needs.occupied_furniture)) {
                        auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                        if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                            keep_furniture = true;
                        }
                    }
                } else if (is_scheduled_eat) {
                    if (reg.valid(needs.occupied_furniture)) {
                        auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                        if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                            keep_furniture = true;
                        }
                    }
                }
            }
            if (!keep_furniture) {
                if (reg.valid(needs.occupied_furniture)) {
                    auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                    aff.occupied_by = entt::null;
                    if (reg.all_of<IdentityComp>(needs.occupied_furniture)) {
                        const auto& id = reg.get<IdentityComp>(needs.occupied_furniture);
                        if (id.display_name.find("임시") != std::string::npos) {
                            entt::entity temp_furn = needs.occupied_furniture;
                            grid.Remove(temp_furn, reg.get<LocationComp>(temp_furn).zone_id);
                            reg.destroy(temp_furn);
                            std::cout << "🔥 [야영 철거] 사용이 끝난 [" << id.display_name << "]을(를) 완전히 철거했습니다." << std::endl;
                        }
                    }
                }
                needs.occupied_furniture = entt::null;
            }
        }

        // 수면 또는 식사를 수행해야 하는 경우
        bool needs_action = needs.is_resolving_survival || is_scheduled_sleep || is_scheduled_eat;
        if (needs_action) {
            // 목적지에 이미 머무르고 있는 상태이고, 작업 중인 상태
            bool at_destination = (loc.location_name == job.target_location);
            if (at_destination) {
                // 가구 점유 시도
                if (needs.occupied_furniture == entt::null) {
                    entt::entity target_furn = entt::null;
                    auto furn_view_inner = reg.view<AffordanceComp, LocationComp, IdentityComp>();
                    furn_view_inner.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc, IdentityComp& furn_id) {
                        if (target_furn != entt::null) return;
                        if (furn_loc.zone_id == loc.zone_id && aff.occupied_by == entt::null) {
                            if (needs.current_survival_type == "hunger" || is_scheduled_eat) {
                                if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                                    target_furn = furn_ent;
                                }
                            } else if (needs.current_survival_type == "fatigue" || is_scheduled_sleep) {
                                if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                                    target_furn = furn_ent;
                                }
                            }
                        }
                    });

                    // 황무지이고 주변에 가구가 없으면 야영지/모닥불 스폰
                    if (target_furn == entt::null && (loc.location_name == "Wilderness" || loc.location_name == "황무지")) {
                        entt::entity camp_ent = reg.create();
                        if (needs.current_survival_type == "hunger" || is_scheduled_eat) {
                            reg.emplace<IdentityComp>(camp_ent, 0u, "임시 모닥불");
                            reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                            reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Eat, entt::null);
                            std::cout << "🔥 [야영 개시] " << identity.display_name << "이(가) 황무지 현위치에 [임시 모닥불]을 피우고 식사 준비를 합니다." << std::endl;
                        } else {
                            reg.emplace<IdentityComp>(camp_ent, 0u, "임시 야영지");
                            reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                            reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Sleep, entt::null);
                            std::cout << "⛺ [야영 개시] " << identity.display_name << "이(가) 황무지 현위치에 [임시 야영지]를 구축하고 잠을 청합니다." << std::endl;
                        }
                        grid.Insert(camp_ent, loc.zone_id);
                        target_furn = camp_ent;
                    }

                    if (target_furn != entt::null) {
                        auto& target_aff = reg.get<AffordanceComp>(target_furn);
                        target_aff.occupied_by = npc_entity;
                        needs.occupied_furniture = target_furn;

                        const auto& furn_loc = reg.get<LocationComp>(target_furn);
                        loc.x = furn_loc.x;
                        loc.y = furn_loc.y;
                        loc.z = furn_loc.z;

                        // 상호작용 Toil 설정
                        if (needs.is_resolving_survival) {
                            toil.state = ToilState::Working;
                            toil.duration_ticks = 9999;
                        }

                        auto& act = reg.get<ActivityComp>(npc_entity);
                        if (needs.current_survival_type == "hunger" || is_scheduled_eat) {
                            toil.current_action = "Eating";
                            act.current_activity = (reg.get<IdentityComp>(target_furn).display_name.find("임시") != std::string::npos) ? "황무지 야영지에서 식사 중" : "식탁에 앉아 식사 중";
                            std::cout << "🍽️ [가구 상호작용] " << identity.display_name << "이(가) [" 
                                      << reg.get<IdentityComp>(target_furn).display_name << "]를 사용하여 식사를 시작합니다." << std::endl;
                        } else {
                            toil.current_action = "Sleeping";
                            act.current_activity = (reg.get<IdentityComp>(target_furn).display_name.find("임시") != std::string::npos) ? "황무지 야영지에서 숙면 중" : "침대에서 숙면 중";
                            std::cout << "💤 [가구 상호작용] " << identity.display_name << "이(가) [" 
                                      << reg.get<IdentityComp>(target_furn).display_name << "]를 사용하여 숙면에 들어갑니다." << std::endl;
                        }
                    } else {
                        // 가구가 없고 황무지도 아닌 경우 맨바닥 노숙
                        if (needs.is_resolving_survival) {
                            toil.state = ToilState::Working;
                            toil.duration_ticks = 9999;
                        }
                        auto& act = reg.get<ActivityComp>(npc_entity);
                        if (needs.current_survival_type == "hunger" || is_scheduled_eat) {
                            toil.current_action = "Eating_On_Floor";
                            act.current_activity = "자리가 부족해 바닥에서 식사 중";
                            std::cout << "🥪 [바닥 취식] " << identity.display_name << "이(가) 빈 의자가 없어 바닥에서 식사합니다." << std::endl;
                        } else {
                            toil.current_action = "Sleeping_On_Floor";
                            act.current_activity = "자리가 부족해 구석에서 선잠 중";
                            std::cout << "⛺ [노숙 취침] " << identity.display_name << "이(가) 빈 침대가 없어 길가에서 잠을 청합니다." << std::endl;
                        }
                    }
                }

                // 점유 상태 또는 맨바닥 회복 진행
                float recovery = 0.0f;
                bool is_eating = (needs.current_survival_type == "hunger" || is_scheduled_eat);
                bool is_sleeping = (needs.current_survival_type == "fatigue" || is_scheduled_sleep);

                if (needs.occupied_furniture != entt::null) {
                    auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                    auto& id_comp = reg.get<IdentityComp>(needs.occupied_furniture);
                    bool is_temp = (id_comp.display_name.find("임시") != std::string::npos);

                    if (is_eating) {
                        recovery = is_temp ? 0.25f : 0.5f;
                    } else if (is_sleeping) {
                        if (is_temp) recovery = 0.0625f;
                        else if (aff.type == AffordanceType::Sit) recovery = 0.083f;
                        else recovery = 0.125f;
                    }
                } else {
                    if (is_eating) recovery = 0.125f;
                    else if (is_sleeping) recovery = 0.025f;
                }

                if (is_eating) {
                    needs.hunger += recovery;
                    if (needs.hunger > 100.0f) needs.hunger = 100.0f;
                }
                if (is_sleeping) {
                    needs.fatigue += recovery;
                    if (needs.fatigue > 100.0f) needs.fatigue = 100.0f;
                }

                // 생체 위기 종료 검사
                if (needs.is_resolving_survival) {
                    bool resolved = false;
                    if (needs.current_survival_type == "hunger" && needs.hunger >= 95.0f) {
                        resolved = true;
                    } else if (needs.current_survival_type == "fatigue" && needs.fatigue >= 95.0f) {
                        resolved = true;
                    }

                    if (resolved) {
                        std::cout << "🔋 [생체 충전 완료] " << identity.display_name << "의 생존 위기 해결! (" 
                                  << needs.current_survival_type << " 완료, 허기: " << needs.hunger << ", 피로: " << needs.fatigue << ")" << std::endl;

                        if (needs.occupied_furniture != entt::null) {
                            auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                            aff.occupied_by = entt::null;
                            if (reg.all_of<IdentityComp>(needs.occupied_furniture)) {
                                const auto& id = reg.get<IdentityComp>(needs.occupied_furniture);
                                if (id.display_name.find("임시") != std::string::npos) {
                                    entt::entity temp_furn = needs.occupied_furniture;
                                    grid.Remove(temp_furn, reg.get<LocationComp>(temp_furn).zone_id);
                                    reg.destroy(temp_furn);
                                    std::cout << "🔥 [야영 철거] 사용이 끝난 [" << id.display_name << "]을(를) 완전히 철거했습니다." << std::endl;
                                }
                            }
                            needs.occupied_furniture = entt::null;
                        }

                        needs.is_resolving_survival = false;
                        needs.current_survival_type = "";

                        uint32_t npc_id = identity.npc_id;
                        uint64_t job_id = job.job_id;
                        client.ReportJobStatusAsync(npc_id, job_id, 0, mundusvivens::UNKNOWN_REASON, "survival_resolved", current_tick,
                            MundusVivens::AsyncGrpcClient::ReportJobStatusCallback([](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                            }));

                        job.is_active = false;
                        toil.state = ToilState::Idle;
                        toil.duration_ticks = 0;
                        toil.current_action = "";
                        auto& act = reg.get<ActivityComp>(npc_entity);
                        act.current_activity = "대기";
                    }
                }
            }
        }
    });
}
