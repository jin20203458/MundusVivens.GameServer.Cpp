#pragma once
#include "BTNode.h"
#include "GrpcResultQueue.h"
#include "Components.h"
#include "Systems.h"
#include "LocationRegistry.h"
#include <iostream>
#include <cmath>

namespace BT {

// 🆕 조건 노드: 허기 상태 검사 및 생체 위기 트리거
class ConditionIsHungry : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        
        // 이미 기아 위기 해결 중이면 Success
        if (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Hunger) {
            return NodeStatus::Success;
        }
        
        // 다른 위기(피로) 해결 중이면 양보
        if (needs.is_resolving_survival) {
            return NodeStatus::Failure;
        }
        
        // 허기 수치 위험선 돌입
        if (needs.hunger < 15.0f) {
            needs.is_resolving_survival = true;
            needs.current_survival_type = SurvivalType::Hunger;
            
            // 바쁜 상태(BusyTag) 해제하여 대피/이동 보장
            if (reg.all_of<BusyTag>(entity)) {
                reg.erase<BusyTag>(entity);
            }
            
            // 기존 경로 및 물리 타겟 초기화
            if (reg.all_of<PathfindingComp>(entity)) {
                auto& path = reg.get<PathfindingComp>(entity);
                path.waypoints.clear();
                path.current_waypoint_index = 0;
            }
            
            auto& identity = reg.get<IdentityComp>(entity);
            auto& job = reg.get<JobComp>(entity);
            auto& toil = reg.get<ToilComp>(entity);
            auto& ctx = reg.ctx().get<BTContext>();
            
            std::cout << "🚨 [BT 기아 위기] " << identity.display_name << "의 허기 상태 위험 돌입! (허기: " 
                      << needs.hunger << "/100)" << std::endl;
            
            // C# 서버 비동기 중단 보고
            uint32_t npc_id = identity.npc_id;
            uint64_t job_id = job.job_id;
            ctx.client->ReportJobStatusAsync(npc_id, job_id, 2, mundusvivens::ENVIRONMENT_CHANGE, "survival_hunger", *ctx.current_tick,
                [&ctx](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                    ctx.grpc_queue->Push([success](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                        if (success) {
                            std::cout << "📥 [BT 기아 보고 완료] C# 서버 수신 및 우회 확인." << std::endl;
                        }
                    });
                });
            
            // C++ 척수 수준 가상 Job 주입
            job.job_id = 999000;
            job.intent = "식사 중";
            job.category = JobCategory::Eat;
            job.is_active = true;
            job.target_location = "";
            job.target_x = 0.0f;
            job.target_y = 0.0f;
            job.target_z = 0.0f;
            
            toil.state = ToilState::Idle;
            toil.duration_ticks = 0;
            toil.current_action = "";
            
            return NodeStatus::Success;
        }
        
        return NodeStatus::Failure;
    }
};

// 🆕 조건 노드: 피로 상태 검사 및 생체 위기 트리거
class ConditionIsFatigued : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        
        // 이미 피로 위기 해결 중이면 Success
        if (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Fatigue) {
            return NodeStatus::Success;
        }
        
        // 다른 위기(허기) 해결 중이면 양보
        if (needs.is_resolving_survival) {
            return NodeStatus::Failure;
        }
        
        // 피로 수치 위험선 돌입
        if (needs.fatigue < 15.0f) {
            needs.is_resolving_survival = true;
            needs.current_survival_type = SurvivalType::Fatigue;
            
            // 바쁜 상태 해제
            if (reg.all_of<BusyTag>(entity)) {
                reg.erase<BusyTag>(entity);
            }
            
            // 경로 초기화
            if (reg.all_of<PathfindingComp>(entity)) {
                auto& path = reg.get<PathfindingComp>(entity);
                path.waypoints.clear();
                path.current_waypoint_index = 0;
            }
            
            auto& identity = reg.get<IdentityComp>(entity);
            auto& job = reg.get<JobComp>(entity);
            auto& toil = reg.get<ToilComp>(entity);
            auto& ctx = reg.ctx().get<BTContext>();
            
            std::cout << "🚨 [BT 피로 위기] " << identity.display_name << "의 피로 상태 위험 돌입! (피로: " 
                      << needs.fatigue << "/100)" << std::endl;
            
            // C# 서버 비동기 중단 보고
            uint32_t npc_id = identity.npc_id;
            uint64_t job_id = job.job_id;
            ctx.client->ReportJobStatusAsync(npc_id, job_id, 2, mundusvivens::ENVIRONMENT_CHANGE, "survival_fatigue", *ctx.current_tick,
                [&ctx](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                    ctx.grpc_queue->Push([success](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                        if (success) {
                            std::cout << "📥 [BT 피로 보고 완료] C# 서버 수신 및 우회 확인." << std::endl;
                        }
                    });
                });
            
            // C++ 척수 수준 가상 Job 주입
            job.job_id = 999001;
            job.intent = "취침 중";
            job.category = JobCategory::Sleep;
            job.is_active = true;
            job.target_location = "";
            job.target_x = 0.0f;
            job.target_y = 0.0f;
            job.target_z = 0.0f;
            
            toil.state = ToilState::Idle;
            toil.duration_ticks = 0;
            toil.current_action = "";
            
            return NodeStatus::Success;
        }
        
        return NodeStatus::Failure;
    }
};

// 🆕 조건 노드: 스케줄 식사 여부 검사 및 생체 위기 트리거
class ConditionIsScheduledEat : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        
        // 이미 해당 위기 해결 중이면 Success
        if (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Hunger) {
            return NodeStatus::Success;
        }
        
        // 다른 위기(피로) 해결 중이면 양보
        if (needs.is_resolving_survival) {
            return NodeStatus::Failure;
        }
        
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& loc = reg.get<LocationComp>(entity);
        
        // 스케줄 Eat 작업이 활성이고, 이미 목적지에 도착하여 Working 중인 경우
        if (job.is_active && job.category == JobCategory::Eat 
            && toil.state == ToilState::Working 
            && loc.location_name == job.target_location) {
            
            needs.is_resolving_survival = true;
            needs.current_survival_type = SurvivalType::Hunger;
            return NodeStatus::Success;
        }
        
        return NodeStatus::Failure;
    }
};

// 🆕 조건 노드: 스케줄 수면 여부 검사 및 생체 위기 트리거
class ConditionIsScheduledSleep : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        
        // 이미 해당 위기 해결 중이면 Success
        if (needs.is_resolving_survival && needs.current_survival_type == SurvivalType::Fatigue) {
            return NodeStatus::Success;
        }
        
        // 다른 위기(허기) 해결 중이면 양보
        if (needs.is_resolving_survival) {
            return NodeStatus::Failure;
        }
        
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& loc = reg.get<LocationComp>(entity);
        
        // 스케줄 Sleep 작업이 활성이고, 이미 목적지에 도착하여 Working 중인 경우
        if (job.is_active && job.category == JobCategory::Sleep 
            && toil.state == ToilState::Working 
            && loc.location_name == job.target_location) {
            
            needs.is_resolving_survival = true;
            needs.current_survival_type = SurvivalType::Fatigue;
            return NodeStatus::Success;
        }
        
        return NodeStatus::Failure;
    }
};

// 🆕 실행 노드: 가까운 가구/사물 찾기 및 이동 타겟 지정
class ActionFindFurniture : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& loc = reg.get<LocationComp>(entity);
        auto& identity = reg.get<IdentityComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();
        
        // 이미 가구 타겟이 설정되었으면 Success
        if (!job.target_location.empty() && (job.target_x != 0.0f || job.target_z != 0.0f)) {
            return NodeStatus::Success;
        }
        
        entt::entity target_furn = entt::null;
        float min_dist = 999999.0f;
        std::string found_loc = "";
        float found_x = 0.0f, found_y = 0.0f, found_z = 0.0f;
        
        auto furn_view = reg.view<AffordanceComp, LocationComp>();
        furn_view.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc) {
            bool matches = false;
            if (needs.current_survival_type == SurvivalType::Hunger) {
                if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                    matches = true;
                }
            } else if (needs.current_survival_type == SurvivalType::Fatigue) {
                if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                    matches = true;
                }
            }
            
            if (matches && aff.occupied_by == entt::null) {
                float dx = furn_loc.x - loc.x;
                float dz = furn_loc.z - loc.z;
                float dist = std::sqrt(dx * dx + dz * dz);
                if (furn_loc.zone_id != loc.zone_id) {
                    dist += 1000.0f; // 다른 존 패널티
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
            toil.state = ToilState::Idle;
            return NodeStatus::Success;
        } else {
            // 가구가 근처에 없음 -> 황무지 야영 스폰 규칙 적용
            if (loc.location_name == "Wilderness" || loc.location_name == "황무지") {
                entt::entity camp_ent = reg.create();
                std::string camp_name = (needs.current_survival_type == SurvivalType::Hunger) ? "임시 모닥불" : "임시 야영지";
                AffordanceType aff_type = (needs.current_survival_type == SurvivalType::Hunger) ? AffordanceType::Eat : AffordanceType::Sleep;
                
                reg.emplace<IdentityComp>(camp_ent, 0u, camp_name);
                reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                auto& camp_aff = reg.emplace<AffordanceComp>(camp_ent, aff_type, entt::null);
                camp_aff.is_temporary = true;
                
                ctx.location_registry->UpdateEntityPosition(camp_ent, loc.x, loc.z, reg);
                
                job.target_location = loc.location_name;
                job.target_x = loc.x;
                job.target_y = loc.y;
                job.target_z = loc.z;
                toil.state = ToilState::Idle;
                
                std::cout << "🔥 [BT 야영 개시] " << identity.display_name << "이(가) 황무지 현위치에 [" 
                          << camp_name << "]을 피우고 생존을 시작합니다." << std::endl;
                          
                return NodeStatus::Success;
            } else {
                // 노숙 모드 결정
                job.target_location = loc.location_name;
                job.target_x = loc.x;
                job.target_y = loc.y;
                job.target_z = loc.z;
                toil.state = ToilState::Idle;
                
                std::cout << "⛺ [BT 노숙 결정] " << identity.display_name << "이(가) 마을 내 빈 가구를 찾지 못해 현위치 노숙을 결정합니다." << std::endl;
                return NodeStatus::Success;
            }
        }
    }
};

// 🆕 실행 노드: 물리적 이동 명령 하달 및 상태 모니터링
class ActionMoveToTarget : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& loc = reg.get<LocationComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        
        // 1. 이미 목적지에 도달했으면 완료 (Success)
        if (loc.location_name == job.target_location) {
            return NodeStatus::Success;
        }
        
        float dx = job.target_x - loc.x;
        float dz = job.target_z - loc.z;
        float dist = std::sqrt(dx * dx + dz * dz);
        if (dist < 0.8f) {
            return NodeStatus::Success;
        }
        
        // 2. 이동 상태가 아니면 이동 시작 명령 하달
        if (toil.state != ToilState::Moving) {
            toil.state = ToilState::Moving;
            act.current_activity = "이동 중: " + job.target_location;
            
            // 기존 경로가 있으면 비워서 새로 찾게 유도
            if (reg.all_of<PathfindingComp>(entity)) {
                auto& path = reg.get<PathfindingComp>(entity);
                path.waypoints.clear();
                path.current_waypoint_index = 0;
            }
            return NodeStatus::Running;
        }
        
        // 3. 이동 중인 경우 상태 체크
        if (reg.all_of<PathfindingComp>(entity)) {
            auto& path = reg.get<PathfindingComp>(entity);
            // SystemPathfinding이 경로를 못 찾아 Idle로 떨어졌다면 실패 판정
            if (toil.state == ToilState::Idle && path.waypoints.empty()) {
                return NodeStatus::Failure;
            }
        }
        
        return NodeStatus::Running;
    }
};

// 🆕 실행 노드: 가구 점유, Toil 설정 및 틱당 욕구 충전
class ActionInteractFurniture : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& needs = reg.get<NeedsComp>(entity);
        auto& loc = reg.get<LocationComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& identity = reg.get<IdentityComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();
        
        bool is_eating = (needs.current_survival_type == SurvivalType::Hunger);
        bool is_sleeping = (needs.current_survival_type == SurvivalType::Fatigue);
        
        // 1. 가구 점유 시도 (최초 프레임)
        if (needs.occupied_furniture == entt::null && 
            toil.current_action != "Eating_On_Floor" && 
            toil.current_action != "Sleeping_On_Floor") {
            entt::entity target_furn = entt::null;
            auto furn_view = reg.view<AffordanceComp, LocationComp, IdentityComp>();
            furn_view.each([&](entt::entity furn_ent, AffordanceComp& aff, LocationComp& furn_loc, IdentityComp& furn_id) {
                if (target_furn != entt::null) return;
                if (furn_loc.zone_id == loc.zone_id && aff.occupied_by == entt::null) {
                    if (is_eating) {
                        if (aff.type == AffordanceType::Eat || aff.type == AffordanceType::Drink || aff.type == AffordanceType::Sit) {
                            target_furn = furn_ent;
                        }
                    } else if (is_sleeping) {
                        if (aff.type == AffordanceType::Sleep || aff.type == AffordanceType::Sit) {
                            target_furn = furn_ent;
                        }
                    }
                }
            });
            
            // 만약 가구가 사라졌거나 점유당했을 경우 황무지면 스폰
            if (target_furn == entt::null && (loc.location_name == "Wilderness" || loc.location_name == "황무지")) {
                entt::entity camp_ent = reg.create();
                if (is_eating) {
                    reg.emplace<IdentityComp>(camp_ent, 0u, "임시 모닥불");
                    reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                    auto& camp_aff = reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Eat, entt::null);
                    camp_aff.is_temporary = true;
                    std::cout << "🔥 [BT 야영 스폰] " << identity.display_name << "이(가) [임시 모닥불]을 새로 피웁니다." << std::endl;
                } else {
                    reg.emplace<IdentityComp>(camp_ent, 0u, "임시 야영지");
                    reg.emplace<LocationComp>(camp_ent, loc.zone_id, loc.location_name, loc.x, loc.y, loc.z);
                    auto& camp_aff = reg.emplace<AffordanceComp>(camp_ent, AffordanceType::Sleep, entt::null);
                    camp_aff.is_temporary = true;
                    std::cout << "⛺ [BT 야영 스폰] " << identity.display_name << "이(가) [임시 야영지]를 새로 구축합니다." << std::endl;
                }
                ctx.location_registry->UpdateEntityPosition(camp_ent, loc.x, loc.z, reg);
                target_furn = camp_ent;
            }
            
            if (target_furn != entt::null) {
                auto& target_aff = reg.get<AffordanceComp>(target_furn);
                target_aff.occupied_by = entity;
                needs.occupied_furniture = target_furn;
                
                // 가구 좌표로 싱크
                const auto& furn_loc = reg.get<LocationComp>(target_furn);
                loc.x = furn_loc.x;
                loc.y = furn_loc.y;
                loc.z = furn_loc.z;
                
                toil.state = ToilState::Working;
                toil.duration_ticks = 9999;
                
                if (is_eating) {
                    toil.current_action = "Eating";
                    act.current_activity = target_aff.is_temporary ? "황무지 야영지에서 식사 중" : "식탁에 앉아 식사 중";
                    std::cout << "🍽️ [BT 가구 상호작용] " << identity.display_name << "이(가) [" 
                              << reg.get<IdentityComp>(target_furn).display_name << "]에서 식사를 시작합니다." << std::endl;
                } else {
                    toil.current_action = "Sleeping";
                    act.current_activity = target_aff.is_temporary ? "황무지 야영지에서 숙면 중" : "침대에서 숙면 중";
                    std::cout << "💤 [BT 가구 상호작용] " << identity.display_name << "이(가) [" 
                              << reg.get<IdentityComp>(target_furn).display_name << "]에서 숙면에 들어갑니다." << std::endl;
                }
            } else {
                // 맨바닥 노숙
                toil.state = ToilState::Working;
                toil.duration_ticks = 9999;
                if (is_eating) {
                    toil.current_action = "Eating_On_Floor";
                    act.current_activity = "자리가 부족해 바닥에서 식사 중";
                    std::cout << "🥪 [BT 바닥 식사] " << identity.display_name << "이(가) 길가 바닥에서 식사합니다." << std::endl;
                } else {
                    toil.current_action = "Sleeping_On_Floor";
                    act.current_activity = "자리가 부족해 구석에서 선잠 중";
                    std::cout << "⛺ [BT 노숙 취침] " << identity.display_name << "이(가) 길가 바닥에서 취침합니다." << std::endl;
                }
            }
            return NodeStatus::Running;
        }
        
        // 2. 매 틱 생체 욕구 충전 진행
        float recovery = 0.0f;
        if (needs.occupied_furniture != entt::null) {
            auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
            bool is_temp = aff.is_temporary;
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
        
        // 3. 충전 완료 검사
        bool resolved = false;
        if (is_eating && needs.hunger >= 95.0f) resolved = true;
        if (is_sleeping && needs.fatigue >= 95.0f) resolved = true;
        
        if (resolved) {
            std::string type_str = is_eating ? "허기" : "피로";
            std::cout << "🔋 [BT 생체 충전 완료] " << identity.display_name << "의 " << type_str << " 해소 완료! (허기: " 
                      << needs.hunger << ", 피로: " << needs.fatigue << ")" << std::endl;
                      
            // 가구 점유 해제 및 임시 가구 철거
            if (needs.occupied_furniture != entt::null) {
                auto& aff = reg.get<AffordanceComp>(needs.occupied_furniture);
                aff.occupied_by = entt::null;
                if (aff.is_temporary) {
                    entt::entity temp_furn = needs.occupied_furniture;
                    std::string furn_name = reg.all_of<IdentityComp>(temp_furn) ? reg.get<IdentityComp>(temp_furn).display_name : "임시 사물";
                    ctx.location_registry->RemoveEntity(temp_furn);
                    reg.destroy(temp_furn);
                    std::cout << "🔥 [BT 야영 철거] 사용 완료된 [" << furn_name << "]을(를) 철거했습니다." << std::endl;
                }
                needs.occupied_furniture = entt::null;
            }
            
            needs.is_resolving_survival = false;
            needs.current_survival_type = SurvivalType::None;
            
            // C# 서버 비동기 완료 보고
            uint32_t npc_id = identity.npc_id;
            uint64_t job_id = job.job_id;
            ctx.client->ReportJobStatusAsync(npc_id, job_id, 0, mundusvivens::UNKNOWN_REASON, "survival_resolved", *ctx.current_tick,
                [](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                });
                
            job.is_active = false;
            toil.state = ToilState::Idle;
            toil.duration_ticks = 0;
            toil.current_action = "";
            act.current_activity = "대기";
            
            return NodeStatus::Success;
        }
        
        return NodeStatus::Running;
    }
};

// 🆕 조건 노드: 전투 타겟 보유 여부 및 유효성 검사
class ConditionHasCombatTarget : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        if (!reg.all_of<CombatComp>(entity)) return NodeStatus::Failure;
        auto& combat = reg.get<CombatComp>(entity);
        if (combat.target_entity == entt::null || !reg.valid(combat.target_entity)) {
            return NodeStatus::Failure;
        }
        if (reg.all_of<HealthComp>(combat.target_entity)) {
            auto& target_health = reg.get<HealthComp>(combat.target_entity);
            if (target_health.is_dead) {
                combat.target_entity = entt::null; // 사망한 타겟 해제
                return NodeStatus::Failure;
            }
        }
        return NodeStatus::Success;
    }
};

// 🆕 조건 노드: 체력 저하 여부 검사 (HP < 30%)
class ConditionIsLowHealth : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        if (!reg.all_of<HealthComp>(entity)) return NodeStatus::Failure;
        auto& health = reg.get<HealthComp>(entity);
        if (health.hp < 30.0f) {
            return NodeStatus::Success;
        }
        return NodeStatus::Failure;
    }
};

// 🆕 실행 노드: 타겟의 반대 방향으로 도주
class ActionFlee : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& loc = reg.get<LocationComp>(entity);
        auto& combat = reg.get<CombatComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();
        auto& identity = reg.get<IdentityComp>(entity);

        if (combat.target_entity == entt::null || !reg.valid(combat.target_entity)) {
            return NodeStatus::Failure;
        }

        auto& target_loc = reg.get<LocationComp>(combat.target_entity);

        // 도주 목적지 재계산 여부 결정 (경로 탐색 스래싱 방지)
        bool need_recalculate = false;
        if (toil.state != ToilState::Moving) {
            need_recalculate = true;
        } else if (reg.all_of<PathfindingComp>(entity)) {
            auto& path = reg.get<PathfindingComp>(entity);
            if (path.waypoints.empty()) {
                need_recalculate = true;
            } else {
                float dist_to_dest = std::sqrt(std::pow(loc.x - job.target_x, 2) + std::pow(loc.z - job.target_z, 2));
                if (dist_to_dest < 2.0f) {
                    need_recalculate = true;
                } else {
                    // 20틱(1초)마다 방향 재보정 (매 프레임 재계산으로 인한 스래싱 방지, 각 엔티티 틱 오프셋 분산)
                    if (ctx.current_tick && ((*ctx.current_tick + static_cast<int>(entity)) % 20 == 0)) {
                        need_recalculate = true;
                    }
                }
            }
        } else {
            need_recalculate = true;
        }

        if (need_recalculate) {
            // 타겟 반대 방향 벡터 계산
            float dx = loc.x - target_loc.x;
            float dz = loc.z - target_loc.z;
            float dist = std::sqrt(dx * dx + dz * dz);

            float dir_x = 1.0f;
            float dir_z = 0.0f;
            if (dist > 0.01f) {
                dir_x = dx / dist;
                dir_z = dz / dist;
            } else {
                // 거리가 너무 가까우면 랜덤 방향 설정
                dir_x = (rand() % 200 - 100) / 100.0f;
                dir_z = (rand() % 200 - 100) / 100.0f;
                float r_dist = std::sqrt(dir_x * dir_x + dir_z * dir_z);
                if (r_dist > 0.0f) {
                    dir_x /= r_dist;
                    dir_z /= r_dist;
                }
            }

            float flee_distance = 15.0f;
            float dest_x = std::clamp(loc.x + dir_x * flee_distance, 5.0f, 1995.0f);
            float dest_z = std::clamp(loc.z + dir_z * flee_distance, 5.0f, 1995.0f);

            // GridMap 경계 제한 및 이동성 확인 (벽 피하기)
            if (ctx.grid_map) {
                int gx = static_cast<int>(dest_x);
                int gz = static_cast<int>(dest_z);
                if (!ctx.grid_map->IsWalkable(gx, gz)) {
                    // 회전 시도 (+45도, -45도 등)
                    float angles[] = { 0.785f, -0.785f, 1.57f, -1.57f };
                    bool found = false;
                    for (float angle : angles) {
                        float rx = dir_x * std::cos(angle) - dir_z * std::sin(angle);
                        float rz = dir_x * std::sin(angle) + dir_z * std::cos(angle);
                        float test_x = std::clamp(loc.x + rx * flee_distance, 5.0f, 1995.0f);
                        float test_z = std::clamp(loc.z + rz * flee_distance, 5.0f, 1995.0f);
                        int tgx = static_cast<int>(test_x);
                        int tgz = static_cast<int>(test_z);
                        if (ctx.grid_map->IsWalkable(tgx, tgz)) {
                            dest_x = test_x;
                            dest_z = test_z;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        dest_x = std::clamp(loc.x, 5.0f, 1995.0f);
                        dest_z = std::clamp(loc.z, 5.0f, 1995.0f);
                    }
                }
            }

            job.target_x = dest_x;
            job.target_z = dest_z;
            job.target_location = "Wilderness";
            
            if (reg.all_of<PathfindingComp>(entity)) {
                auto& path = reg.get<PathfindingComp>(entity);
                path.waypoints.clear();
                path.current_waypoint_index = 0;
            }
            
            toil.state = ToilState::Moving;
            act.current_activity = "도망치는 중!";
            std::cout << "🏃 [BT 도주] " << identity.display_name << "이(가) 위험을 느끼고 (" 
                      << dest_x << ", " << dest_z << ") 방향으로 도망칩니다." << std::endl;
        }

        return NodeStatus::Running;
    }
};

// 🆕 조건 노드: 배회 가능 상태(Idle) 검사
class ConditionCanWander : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        // C# 고차원 Job이 활성화되어 있으면 배회 금지
        if (reg.all_of<JobComp>(entity)) {
            auto& job = reg.get<JobComp>(entity);
            if (job.is_active) {
                return NodeStatus::Failure;
            }
        }
        
        // 생체 위기 대처 중이면 배회 금지
        if (reg.all_of<NeedsComp>(entity)) {
            auto& needs = reg.get<NeedsComp>(entity);
            if (needs.is_resolving_survival) {
                return NodeStatus::Failure;
            }
        }

        // 전투 중이면 배회 금지
        if (reg.all_of<CombatComp>(entity)) {
            auto& combat = reg.get<CombatComp>(entity);
            if (combat.target_entity != entt::null && reg.valid(combat.target_entity)) {
                return NodeStatus::Failure;
            }
        }

        // 바쁜 상태면 배회 금지
        if (reg.all_of<BusyTag>(entity)) {
            return NodeStatus::Failure;
        }

        return NodeStatus::Success;
    }
};

// 🆕 실행 노드: 서식지 내 배회
class ActionWander : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& loc = reg.get<LocationComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();
        auto& identity = reg.get<IdentityComp>(entity);

        // 1. 이미 이동 중인 경우 상태 유지
        if (toil.state == ToilState::Moving) {
            return NodeStatus::Running;
        }

        // 2. 목적지 도달하여 대기(Working) 중인 경우
        if (toil.state == ToilState::Working && job.intent == "배회 중") {
            if (toil.duration_ticks > 0) {
                toil.duration_ticks--;
                return NodeStatus::Running;
            } else {
                // 배회 완료
                job.intent = "";
                toil.state = ToilState::Idle;
                toil.current_action = "";
                act.current_activity = "대기";
                return NodeStatus::Success;
            }
        }

        // 3. 대기 상태일 때만 새로운 배회 목표 설정
        if (toil.state == ToilState::Idle) {
            // 배회 쿨타임 주기 위해 무작위 대기 (약 2.5% 확률로만 배회 시작, 2초에 한 번 꼴)
            if (rand() % 40 != 0) {
                toil.state = ToilState::Idle;
                act.current_activity = "대기";
                return NodeStatus::Running; 
            }

            float wander_range = 15.0f;
            float target_x = 0.0f;
            float target_z = 0.0f;
            bool found = false;

            // 최대 10회 랜덤 좌표 탐색
            for (int i = 0; i < 10; ++i) {
                float rx = (rand() % 200 - 100) / 100.0f * wander_range;
                float rz = (rand() % 200 - 100) / 100.0f * wander_range;
                float tx = std::clamp(loc.x + rx, 5.0f, 1995.0f);
                float tz = std::clamp(loc.z + rz, 5.0f, 1995.0f);

                if (ctx.grid_map && ctx.grid_map->IsWalkable(static_cast<int>(tx), static_cast<int>(tz))) {
                    // 서식지(location_name) 유지성 검증
                    if (ctx.location_registry) {
                        std::string target_loc = ctx.location_registry->GetLocationNameAt(tx, tz);
                        if (target_loc == loc.location_name) {
                            target_x = tx;
                            target_z = tz;
                            found = true;
                            break;
                        }
                    } else {
                        target_x = tx;
                        target_z = tz;
                        found = true;
                        break;
                    }
                }
            }

            if (found) {
                job.target_x = target_x;
                job.target_z = target_z;
                job.target_location = loc.location_name;
                job.intent = "배회 중";
                job.priority = 0;
                job.is_active = false; // C# Job이 아니므로 false 유지

                if (reg.all_of<PathfindingComp>(entity)) {
                    auto& path = reg.get<PathfindingComp>(entity);
                    path.waypoints.clear();
                    path.current_waypoint_index = 0;
                }

                toil.state = ToilState::Moving;
                toil.current_action = "배회 중";
                act.current_activity = "주변 배회 중";
                std::cout << "🌲 [BT 배회 시작] " << identity.display_name << "이(가) 서식지 [" 
                          << loc.location_name << "] 내에서 (" << target_x << ", " << target_z << ")로 배회를 시작합니다." << std::endl;
                return NodeStatus::Running;
            }
        }

        return NodeStatus::Failure;
    }
};

// 🆕 실행 노드: 타겟 공격 또는 추격
class ActionMeleeAttack : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        auto& loc = reg.get<LocationComp>(entity);
        auto& combat = reg.get<CombatComp>(entity);
        auto& job = reg.get<JobComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        auto& identity = reg.get<IdentityComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();

        if (combat.target_entity == entt::null || !reg.valid(combat.target_entity)) {
            return NodeStatus::Failure;
        }

        auto& target_loc = reg.get<LocationComp>(combat.target_entity);
        auto& target_health = reg.get<HealthComp>(combat.target_entity);
        auto& target_ident = reg.get<IdentityComp>(combat.target_entity);

        float dx = target_loc.x - loc.x;
        float dz = target_loc.z - loc.z;
        float dist = std::sqrt(dx * dx + dz * dz);

        // 1. 공격 사거리 밖 -> 추격 이동
        if (dist > combat.attack_range) {
            if (std::abs(job.target_x - target_loc.x) > 1.5f || std::abs(job.target_z - target_loc.z) > 1.5f) {
                job.target_x = target_loc.x;
                job.target_z = target_loc.z;
                job.target_location = target_loc.location_name.empty() ? "Wilderness" : target_loc.location_name;

                if (reg.all_of<PathfindingComp>(entity)) {
                    auto& path = reg.get<PathfindingComp>(entity);
                    path.waypoints.clear();
                    path.current_waypoint_index = 0;
                }

                toil.state = ToilState::Moving;
                act.current_activity = "추격 중: " + target_ident.display_name;
            }
            return NodeStatus::Running;
        }

        // 2. 공격 사거리 내 -> 공격 수행
        if (toil.state == ToilState::Moving) {
            toil.state = ToilState::Working;
            if (reg.all_of<VelocityComp>(entity)) {
                auto& vel = reg.get<VelocityComp>(entity);
                vel.dir_x = 0.0f;
                vel.dir_z = 0.0f;
                vel.speed = 0.0f;
            }
        }

        // 쿨다운 관리
        if (combat.cooldown_ticks > 0) {
            combat.cooldown_ticks--;
            act.current_activity = "대치 중: " + target_ident.display_name;
            return NodeStatus::Running;
        }

        // 실제 피해 적용
        target_health.hp -= combat.attack_damage;
        combat.cooldown_ticks = 20; // 20틱(1초) 쿨다운
        act.current_activity = "공격 중: " + target_ident.display_name;

        std::cout << "⚔️ [BT 공격] " << identity.display_name << "이(가) " 
                  << target_ident.display_name << "을(를) 공격! (데미지: " << combat.attack_damage 
                  << ", 남은 HP: " << target_health.hp << "/" << target_health.max_hp << ")" << std::endl;

        // 🆕 C# 대뇌에 피격 사건 보고
        ctx.client->ReportCombatEventAsync(identity.npc_id, target_ident.npc_id, combat.attack_damage, "MeleeWeapon",
            [](bool success) {
                // 피격 보고 비동기 완료 콜백
            });

        if (reg.all_of<BusyTag>(combat.target_entity)) {
            reg.erase<BusyTag>(combat.target_entity);
            std::cout << "💥 [BT 인터럽트] 피격당한 " << target_ident.display_name << "의 행동이 중단되었습니다." << std::endl;
        }

        if (reg.all_of<CombatComp>(combat.target_entity)) {
            auto& target_combat = reg.get<CombatComp>(combat.target_entity);
            if (target_combat.target_entity == entt::null) {
                target_combat.target_entity = entity;
                std::cout << "👿 [BT 보복 타겟팅] " << target_ident.display_name << "이(가) 공격자 " 
                          << identity.display_name << "을(를) 보복 타겟으로 지정합니다." << std::endl;
            }
        }

        if (target_health.hp <= 0.0f) {
            target_health.hp = 0.0f;
            target_health.is_dead = true;
            combat.target_entity = entt::null;
            std::cout << "💀 [BT 사망] " << target_ident.display_name << "이(가) 사망하였습니다." << std::endl;
        }

        return NodeStatus::Success;
    }
};

// 🆕 조건 노드: 적대감(어그로)이 폭발하여 충동을 유발할 만큼 높은지 검사
class ConditionIsAggroHigh : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        // 이미 명시적인 전투 타겟이 있다면 이 노드는 성공으로 흘리지 않고 Failure 반환
        if (reg.all_of<CombatComp>(entity)) {
            auto& combat = reg.get<CombatComp>(entity);
            if (combat.target_entity != entt::null && reg.valid(combat.target_entity)) {
                return NodeStatus::Failure; 
            }
        }
        
        if (!reg.all_of<AggroComp>(entity)) return NodeStatus::Failure;
        auto& aggro = reg.get<AggroComp>(entity);
        
        if (aggro.aggro_score >= 100 && aggro.threat_entity != entt::null && reg.valid(aggro.threat_entity)) {
            return NodeStatus::Success;
        }
        return NodeStatus::Failure;
    }
};

// 🆕 실행 노드: 어그로 대상을 확인하고, 지성체일 경우 대뇌의 억제를 받거나, 몬스터일 경우 즉시 공격 개시
class ActionInhibitOrAttack : public BTNode {
public:
    NodeStatus Tick(entt::registry& reg, entt::entity entity) override {
        if (!reg.all_of<AggroComp>(entity)) return NodeStatus::Failure;
        auto& aggro = reg.get<AggroComp>(entity);
        auto& identity = reg.get<IdentityComp>(entity);
        auto& toil = reg.get<ToilComp>(entity);
        auto& act = reg.get<ActivityComp>(entity);
        auto& ctx = reg.ctx().get<BTContext>();

        entt::entity threat = aggro.threat_entity;
        if (threat == entt::null || !reg.valid(threat)) {
            aggro.aggro_score = 0;
            aggro.is_inhibited = false;
            return NodeStatus::Failure;
        }

        auto& threat_identity = reg.get<IdentityComp>(threat);

        // 1. 비지성체(몬스터/동물)라면 억제 없이 즉시 공격 개시
        bool is_sentient = true;
        if (reg.all_of<SentienceComp>(entity)) {
            is_sentient = reg.get<SentienceComp>(entity).is_sentient;
        }

        if (!is_sentient) {
            auto& combat = reg.get_or_emplace<CombatComp>(entity);
            combat.target_entity = threat;
            aggro.aggro_score = 0; // 어그로 리셋 (실제 전투 돌입)
            std::cout << "🐺 [BT 몬스터 본능] " << identity.display_name << "이(가) 침입자 " 
                      << threat_identity.display_name << "을(를) 본능적으로 즉시 습격합니다!" << std::endl;
            
            toil.state = ToilState::Idle;
            toil.duration_ticks = 0;
            return NodeStatus::Success;
        }

        // 2. 지성체(인간형)라면 이성적 억제 판단 프로세스 돌입
        if (!aggro.is_inhibited) {
            // 최초 충동 억제 진입 -> C# 대뇌에 판단 승인 요청
            aggro.is_inhibited = true;
            aggro.inhibit_wait_ticks = 0;
            
            std::cout << "🧠 [BT 충동 억제] " << identity.display_name << "이(가) " 
                      << threat_identity.display_name << "에 대한 적개심(Aggro: 100)으로 무기에 손을 올리며, 대뇌(C#) 판단을 대기합니다." << std::endl;

            uint32_t npc_id = identity.npc_id;
            uint32_t target_npc_id = threat_identity.npc_id;

            // gRPC 비동기 호출
            ctx.client->ThreatDetectedAsync(npc_id, target_npc_id, aggro.aggro_score,
                [&ctx, entity, target_npc_id](bool success, int action_code) {
                    ctx.grpc_queue->Push([success, action_code, entity, target_npc_id](entt::registry& inner_reg, TcpServer& inner_tcp, MundusVivens::AsyncGrpcClient& inner_client) {
                        if (!inner_reg.valid(entity) || !inner_reg.all_of<AggroComp>(entity)) return;
                        auto& inner_aggro = inner_reg.get<AggroComp>(entity);
                        auto& inner_ident = inner_reg.get<IdentityComp>(entity);
                        auto& ctx_inner = inner_reg.ctx().get<BTContext>();

                        if (success) {
                            if (action_code == 0) { // APPROVE (공격)
                                auto& combat = inner_reg.get_or_emplace<CombatComp>(entity);
                                combat.target_entity = inner_aggro.threat_entity;
                                inner_aggro.aggro_score = 0;
                                inner_aggro.is_inhibited = false;
                                std::cout << "⚔️ [BT 억제 해제] C# 승인 하달! " << inner_ident.display_name 
                                          << "이(가) 무기를 빼 들고 선제 공격을 감행합니다!" << std::endl;
                            } else if (action_code == 2) { // SOCIALIZE (대화/시비)
                                inner_aggro.aggro_score = 0; // 어그로 리셋
                                inner_aggro.is_inhibited = false;
                                
                                // 30초(600틱) 동안 동일 대상에 대한 어그로 면제 설정 (gRPC 스팸 방지)
                                auto& cd = inner_reg.get_or_emplace<CooldownComp>(entity);
                                cd.cooldown_per_target[target_npc_id] = *ctx_inner.current_tick + 600;

                                std::cout << "🗣️ [BT 이성 복구] C# 명령: 말싸움 유도! " << inner_ident.display_name 
                                          << "이(가) 공격 충동을 억제하고 시비를 걸러 다가갑니다." << std::endl;
                            } else { // REJECT (참기/도주)
                                inner_aggro.aggro_score = 0;
                                inner_aggro.is_inhibited = false;
                                
                                // 30초(600틱) 동안 동일 대상에 대한 어그로 면제 설정 (gRPC 스팸 방지)
                                auto& cd = inner_reg.get_or_emplace<CooldownComp>(entity);
                                cd.cooldown_per_target[target_npc_id] = *ctx_inner.current_tick + 600;

                                std::cout << "🛡️ [BT 참기] C# 명령: 참거나 회피! " << inner_ident.display_name 
                                          << "이(가) 화를 삭이고 전투를 회피합니다." << std::endl;
                            }
                        } else {
                            // gRPC 실패 시 안전하게 억제 복구하고 틱 타임아웃에 맡김
                            inner_aggro.is_inhibited = false;
                        }
                    });
                });
        }

        // C# 응답 대기 중 (최대 100틱 = 5초 타임아웃 방지)
        aggro.inhibit_wait_ticks++;
        if (aggro.inhibit_wait_ticks > 100) {
            std::cout << "⏳ [BT 억제 타임아웃] C# 대뇌 응답 지연으로 " << identity.display_name 
                      << "의 적대 억제가 자동으로 풀려 화를 누그러뜨립니다." << std::endl;
            aggro.aggro_score = 0;
            aggro.is_inhibited = false;
            return NodeStatus::Failure;
        }

        toil.state = ToilState::Interrupted;
        act.current_activity = threat_identity.display_name + "와 대치 중 (생각하는 중)";
        return NodeStatus::Running;
    }
};

// 🆕 생존/전투 행동 트리 조립 팩토리 함수
inline std::unique_ptr<BTNode> CreateSurvivalTree() {
    // Sequence (도주: 타겟 있음 + HP 낮음 ➔ 도주)
    auto flee_seq = std::make_unique<Sequence>();
    flee_seq->AddChild(std::make_unique<ConditionHasCombatTarget>());
    flee_seq->AddChild(std::make_unique<ConditionIsLowHealth>());
    flee_seq->AddChild(std::make_unique<ActionFlee>());

    // 🆕 Sequence (어그로 충동-억제 프로세스)
    auto aggro_seq = std::make_unique<Sequence>();
    aggro_seq->AddChild(std::make_unique<ConditionIsAggroHigh>());
    aggro_seq->AddChild(std::make_unique<ActionInhibitOrAttack>());

    // Sequence (전투: 타겟 있음 ➔ 추격/공격)
    auto combat_seq = std::make_unique<Sequence>();
    combat_seq->AddChild(std::make_unique<ConditionHasCombatTarget>());
    combat_seq->AddChild(std::make_unique<ActionMeleeAttack>());

    // Sequence (기아 위기 해결)
    auto hunger_seq = std::make_unique<Sequence>();
    hunger_seq->AddChild(std::make_unique<ConditionIsHungry>());
    hunger_seq->AddChild(std::make_unique<ActionFindFurniture>());
    hunger_seq->AddChild(std::make_unique<ActionMoveToTarget>());
    hunger_seq->AddChild(std::make_unique<ActionInteractFurniture>());
    
    // Sequence (피로 위기 해결)
    auto fatigue_seq = std::make_unique<Sequence>();
    fatigue_seq->AddChild(std::make_unique<ConditionIsFatigued>());
    fatigue_seq->AddChild(std::make_unique<ActionFindFurniture>());
    fatigue_seq->AddChild(std::make_unique<ActionMoveToTarget>());
    fatigue_seq->AddChild(std::make_unique<ActionInteractFurniture>());

    // Sequence (스케줄 식사)
    auto sched_eat_seq = std::make_unique<Sequence>();
    sched_eat_seq->AddChild(std::make_unique<ConditionIsScheduledEat>());
    sched_eat_seq->AddChild(std::make_unique<ActionInteractFurniture>());

    // Sequence (스케줄 수면)
    auto sched_sleep_seq = std::make_unique<Sequence>();
    sched_sleep_seq->AddChild(std::make_unique<ConditionIsScheduledSleep>());
    sched_sleep_seq->AddChild(std::make_unique<ActionInteractFurniture>());

    // 🆕 Sequence (배회)
    auto wander_seq = std::make_unique<Sequence>();
    wander_seq->AddChild(std::make_unique<ConditionCanWander>());
    wander_seq->AddChild(std::make_unique<ActionWander>());
    
    // 루트 Selector (도주 및 충동 판단이 최우선)
    auto root = std::make_unique<Selector>();
    root->AddChild(std::move(flee_seq));
    root->AddChild(std::move(aggro_seq));    // 🆕 2순위: 어그로 감지 및 대뇌 억제/승인
    root->AddChild(std::move(combat_seq));   // 🆕 3순위: 승인되어 셋업된 전투 수행
    root->AddChild(std::move(hunger_seq));
    root->AddChild(std::move(fatigue_seq));
    root->AddChild(std::move(sched_eat_seq));
    root->AddChild(std::move(sched_sleep_seq));
    root->AddChild(std::move(wander_seq));    // 🆕 8순위: 아무것도 안 할 때의 배회
    return root;
}

} // namespace BT
