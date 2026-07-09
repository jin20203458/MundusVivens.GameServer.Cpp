#include "Systems.h"
#include "Components.h"
#include "GrpcResultQueue.h"
#include <iostream>
#include <cmath>

//  생체 욕구 감쇠 시스템 (기존 SystemSurvivalOverride를 경량화)
void SystemSurvivalOverride(entt::registry& reg, LocationRegistry& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    auto view = reg.view<NeedsComp, LocationComp, ToilComp, JobComp>();
    view.each([&](entt::entity entity, NeedsComp& needs, LocationComp& loc, ToilComp& toil, JobComp& job) {
        // 계획 수면/식사 여부 판별
        bool is_scheduled_sleep = false;
        bool is_scheduled_eat = false;
        if (job.is_active && toil.state == ToilState::Working) {
            if (job.category == JobCategory::Sleep) {
                is_scheduled_sleep = true;
            } else if (job.category == JobCategory::Eat) {
                is_scheduled_eat = true;
            }
        }

        // 매 물리 틱마다 욕구 감소 연산 (20Hz 기준 서서히 감쇠)
        // 대뇌 락 중이거나 계획 활동/로컬 BT를 통해 해결 중인 생체 욕구만 감쇠를 면제함
        bool skip_hunger_decay = (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Hunger) || (is_scheduled_eat && needs.occupied_furniture != entt::null);
        bool skip_fatigue_decay = (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Fatigue) || (is_scheduled_sleep && needs.occupied_furniture != entt::null);

        if (!skip_hunger_decay) {
            needs.hunger -= 0.045f;
        }
        if (!skip_fatigue_decay) {
            needs.fatigue -= 0.025f;
        }

        if (needs.hunger < 0.0f) needs.hunger = 0.0f;
        if (needs.fatigue < 0.0f) needs.fatigue = 0.0f;
    });
}

//  사물 상호작용 및 욕구 충전 시스템 (일반 계획 스케줄 전용으로 축소)
void SystemAffordanceResolver(entt::registry& reg, LocationRegistry& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
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

    // 2. C# 스케줄을 처리하고 있는 NPC들의 상태 조율 (BT 기아/피로 위기 해결 중이 아닐 때만 작동)
    auto npc_view = reg.view<NeedsComp, LocationComp, ToilComp, JobComp, IdentityComp>();
    npc_view.each([&](entt::entity npc_entity, NeedsComp& needs, LocationComp& loc, ToilComp& toil, JobComp& job, IdentityComp& identity) {
        if (needs.is_resolving_survival) {
            return; // 위기 상태는 BT가 전담 처리하므로 스킵
        }

        // 계획 수면/식사 여부 판별
        bool is_scheduled_sleep = (job.is_active && toil.state == ToilState::Working && job.category == JobCategory::Sleep);
        bool is_scheduled_eat = (job.is_active && toil.state == ToilState::Working && job.category == JobCategory::Eat);

        // 일반 계획 완료/취소/교체 시 점유 중인 가구 자동 반납 및 철거
        if (needs.occupied_furniture != entt::null) {
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
                            grid.RemoveEntity(temp_furn);
                            reg.destroy(temp_furn);
                            std::cout << "🔥 [야영 철거] 사용이 끝난 [" << id.display_name << "]을(를) 완전히 철거했습니다." << std::endl;
                        }
                    }
                }
                needs.occupied_furniture = entt::null;
            }
        }

        // 수면 또는 식사를 수행해야 하는 경우 (스케줄)
        bool needs_action = is_scheduled_sleep || is_scheduled_eat;
        if (needs_action) {
            bool at_destination = (loc.location_name == job.target_location);
            if (at_destination) {
                // 가구 점유 시도
                if (needs.occupied_furniture == entt::null) {
                    entt::entity target_furn = entt::null;
                    auto furn_view_inner = reg.view<AffordanceComp, LocationComp, IdentityComp>();
                    furn_view_inner.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc, IdentityComp& furn_id) {
                        if (target_furn != entt::null) return;
                        if (furn_loc.zone_id == loc.zone_id && aff.occupied_by == entt::null) {
                            if (is_scheduled_eat) {
                                if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                                    target_furn = furn_ent;
                                }
                            } else if (is_scheduled_sleep) {
                                if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                                    target_furn = furn_ent;
                                }
                            }
                        }
                    });

                    // 황무지이고 주변에 가구가 없으면 야영지/모닥불 스폰
                    if (target_furn == entt::null && (loc.location_name == "Wilderness" || loc.location_name == "황무지")) {
                        entt::entity camp_ent = reg.create();
                        if (is_scheduled_eat) {
                            reg.emplace<IdentityComp>(camp_ent, 0u, "임시 모닥불");
                            reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                            auto& camp_aff = reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Eat, entt::null);
                            camp_aff.is_temporary = true;
                        } else {
                            reg.emplace<IdentityComp>(camp_ent, 0u, "임시 야영지");
                            reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                            auto& camp_aff = reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Sleep, entt::null);
                            camp_aff.is_temporary = true;
                        }
                        grid.UpdateEntityPosition(camp_ent, loc.x, loc.z, reg);
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

                        auto& act = reg.get<ActivityComp>(npc_entity);
                        if (is_scheduled_eat) {
                            toil.current_action = "Eating";
                            act.current_activity = reg.get<AffordanceComp>(target_furn).is_temporary ? "황무지 야영지에서 식사 중" : "식탁에 앉아 식사 중";
                        } else {
                            toil.current_action = "Sleeping";
                            act.current_activity = reg.get<AffordanceComp>(target_furn).is_temporary ? "황무지 야영지에서 숙면 중" : "침대에서 숙면 중";
                        }
                    } else {
                        // 맨바닥 노숙
                        auto& act = reg.get<ActivityComp>(npc_entity);
                        if (is_scheduled_eat) {
                            toil.current_action = "Eating_On_Floor";
                            act.current_activity = "자리가 부족해 바닥에서 식사 중";
                        } else {
                            toil.current_action = "Sleeping_On_Floor";
                            act.current_activity = "자리가 부족해 구석에서 선잠 중";
                        }
                    }
                }

                // 점유 상태 또는 맨바닥 회복 진행
                float recovery = 0.0f;
                if (needs.occupied_furniture != entt::null) {
                    auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                    bool is_temp = aff.is_temporary;

                    if (is_scheduled_eat) {
                        recovery = is_temp ? 0.25f : 0.5f;
                    } else if (is_scheduled_sleep) {
                        if (is_temp) recovery = 0.0625f;
                        else if (aff.type == AffordanceType::Sit) recovery = 0.083f;
                        else recovery = 0.125f;
                    }
                } else {
                    if (is_scheduled_eat) recovery = 0.125f;
                    else if (is_scheduled_sleep) recovery = 0.025f;
                }

                if (is_scheduled_eat) {
                    needs.hunger += recovery;
                    if (needs.hunger > 100.0f) needs.hunger = 100.0f;
                }
                if (is_scheduled_sleep) {
                    needs.fatigue += recovery;
                    if (needs.fatigue > 100.0f) needs.fatigue = 100.0f;
                }
            }
        }
    });
}
