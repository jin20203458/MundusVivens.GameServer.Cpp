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
    auto npc_view = reg.view<NeedsComp, ToilComp, JobComp>();
    npc_view.each([&]([[maybe_unused]] entt::entity npc_entity, NeedsComp& needs, ToilComp& toil, JobComp& job) {
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
    });
}
