#pragma once
#include "BTNode.h"
#include "GrpcResultQueue.h"
#include "Components.h"
#include "Systems.h"
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
            job.is_active = true;
            
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
            job.is_active = true;
            
            toil.state = ToilState::Idle;
            toil.duration_ticks = 0;
            toil.current_action = "";
            
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
                
                ctx.spatial_grid->Insert(camp_ent, loc.zone_id);
                
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
        if (needs.occupied_furniture == entt::null) {
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
                ctx.spatial_grid->Insert(camp_ent, loc.zone_id);
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
                    ctx.spatial_grid->Remove(temp_furn, reg.get<LocationComp>(temp_furn).zone_id);
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

// 🆕 생존 행동 트리 조립 팩토리 함수
inline std::unique_ptr<BTNode> CreateSurvivalTree() {
    // Sequence(배고픔? ➔ 가구찾기 ➔ 이동 ➔ 식사)
    auto hunger_seq = std::make_unique<Sequence>();
    hunger_seq->AddChild(std::make_unique<ConditionIsHungry>());
    hunger_seq->AddChild(std::make_unique<ActionFindFurniture>());
    hunger_seq->AddChild(std::make_unique<ActionMoveToTarget>());
    hunger_seq->AddChild(std::make_unique<ActionInteractFurniture>());
    
    // Sequence(피로? ➔ 가구찾기 ➔ 이동 ➔ 취침)
    auto fatigue_seq = std::make_unique<Sequence>();
    fatigue_seq->AddChild(std::make_unique<ConditionIsFatigued>());
    fatigue_seq->AddChild(std::make_unique<ActionFindFurniture>());
    fatigue_seq->AddChild(std::make_unique<ActionMoveToTarget>());
    fatigue_seq->AddChild(std::make_unique<ActionInteractFurniture>());
    
    // Selector(기아 시퀀스, 피로 시퀀스)
    auto survival_root = std::make_unique<Selector>();
    survival_root->AddChild(std::move(hunger_seq));
    survival_root->AddChild(std::move(fatigue_seq));
    
    return survival_root;
}

} // namespace BT
