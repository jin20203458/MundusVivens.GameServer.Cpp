#include "Systems.h"
#include "Components.h"
#include <iostream>
#include <cmath>
#include "TracyIntegration.h"

// A* 길찾기 적용 시스템
void SystemPathfinding(entt::registry& reg, const GridMap& map) {
    ZoneScoped;
    auto view = reg.view<JobComp, ToilComp, LocationComp>();
    view.each([&](entt::entity entity, JobComp& job, ToilComp& toil, LocationComp& loc) {
        if (toil.state == ToilState::Moving) {
            auto& pathfinding = reg.get_or_emplace<PathfindingComp>(entity);

            float target_x = job.target_x;
            float target_z = job.target_z;

            // target_location 이름이 있고 좌표가 (0,0)인 경우 사전 조회
            if (!job.target_location.empty() && target_x == 0.0f && target_z == 0.0f) {
                map.GetLocationCoords(job.target_location, target_x, target_z);
                // Zone 중심 겹침 방지: 반경 내 랜덤 좌표로 분산
                LocationRegistry::RandomizeWithinRadius(target_x, target_z, LocationRegistry::LOCATION_RADIUS, target_x, target_z);
                job.target_x = target_x;
                job.target_z = target_z;
            }

            // 1. Pathfinding Thresholding: 타겟이 마지막 경로 계산 시점보다 2.0 유닛 이상 움직였을 경우 경로 무효화
            float t_dx = target_x - pathfinding.last_target_x;
            float t_dz = target_z - pathfinding.last_target_z;
            if (t_dx * t_dx + t_dz * t_dz > 4.0f) { // 2.0^2
                pathfinding.waypoints.clear();
            }

            // 2. Line-of-Sight (LOS) 체크: 시야가 확보되면 A* 생략하고 직접 조향(Direct-Seek) 하도록 함
            bool is_blocked = map.IsPathBlocked(loc.x, loc.z, target_x, target_z);
            if (!is_blocked) {
                // 직접 추격할 것이므로 경로 비우기
                if (!pathfinding.waypoints.empty()) {
                    pathfinding.waypoints.clear();
                }
                pathfinding.current_waypoint_index = 0;
                pathfinding.last_target_x = target_x;
                pathfinding.last_target_z = target_z;

                auto& vel = reg.get_or_emplace<VelocityComp>(entity);
                float move_speed = 2.0f;
                if (reg.ctx().contains<SimulationSettings>()) {
                    move_speed = reg.ctx().get<SimulationSettings>().npc_speed;
                }
                vel.speed = move_speed;
            } else {
                // 시야가 막혀있는데 경로가 비어있다면 A* 수행
                if (pathfinding.waypoints.empty()) {
                    pathfinding.waypoints = map.FindPath(loc.x, loc.z, target_x, target_z);
                    pathfinding.current_waypoint_index = 0;
                    pathfinding.last_target_x = target_x;
                    pathfinding.last_target_z = target_z;

                    if (!pathfinding.waypoints.empty()) {
                        auto& vel = reg.get_or_emplace<VelocityComp>(entity);
                        float move_speed = 2.0f;
                        if (reg.ctx().contains<SimulationSettings>()) {
                            move_speed = reg.ctx().get<SimulationSettings>().npc_speed;
                        }
                        vel.speed = move_speed;
                        
                        // if (auto* ident = reg.try_get<IdentityComp>(entity)) {
                        //     std::cout << "🧭 [경로 생성] " << ident->display_name << "이(가) [" << job.target_location << "] (" << target_x << ", " << target_z << ")로의 경로를 A*로 탐색하여 " << pathfinding.waypoints.size() << "개의 노드를 찾았습니다." << std::endl;
                        // }
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
        }
    });
}

// 🆕 Axis 3: 실시간 20Hz 이동 처리 시스템
void SystemMovement(entt::registry& reg, LocationRegistry& grid, const GridMap& map, int tick) {
    ZoneScoped;
    constexpr float dt = 0.05f; // 20Hz 기준 dt

    auto view = reg.view<LocationComp, ToilComp, PathfindingComp, VelocityComp, JobComp, IdentityComp>();
    view.each([&](entt::entity entity, LocationComp& loc, ToilComp& toil, PathfindingComp& path, VelocityComp& vel, JobComp& job, IdentityComp& identity) {
        if (toil.state == ToilState::Moving) {
            // 1. Direct-Seek 모드일 때 (A* 경로가 빈 상태로 눈에 보일 때 직접 돌진)
            if (path.waypoints.empty()) {
                float dx = job.target_x - loc.x;
                float dz = job.target_z - loc.z;
                float dist = std::sqrt(dx * dx + dz * dz);

                // 도착 판정 임계값 (0.8m 이내 도달 시 완수)
                if (dist < 0.8f) {
                    loc.x = job.target_x;
                    loc.y = job.target_y;
                    loc.z = job.target_z;

                    grid.UpdateEntityPosition(entity, loc.x, loc.z, reg);

                    toil.state = ToilState::Working;
                    toil.duration_ticks = 3;
                    toil.current_action = job.intent;
                    
                    auto& act = reg.get<ActivityComp>(entity);
                    act.current_activity = job.intent;

                    vel.dir_x = 0.0f;
                    vel.dir_z = 0.0f;
                    vel.speed = 0.0f;
                    path.current_waypoint_index = 0;

                    bool is_sentient = true;
                    if (reg.all_of<SentienceComp>(entity)) {
                        is_sentient = reg.get<SentienceComp>(entity).is_sentient;
                    }
                    std::string verb = is_sentient ? "작업" : "행동";
                    
                    std::cout << "🏁 [목적지 도착 - Direct Seek] " << identity.display_name 
                              << "이(가) 목적지 [" << loc.location_name << "]에 도달하여 " << verb << "을(를) 시작합니다." << std::endl;
                    std::cout << "🔄 [Toil Transition] " << identity.display_name 
                              << ": Moving ➔ Working (Direct Seek 완료, Job ID: " << job.job_id 
                              << ", Intent: " << job.intent << ")" << std::endl;
                    return;
                }

                // 조향 벡터 계산
                float steer_x = dx / dist;
                float steer_z = dz / dist;

                // 장애물 우회 조향력 (Separation Force): 1.5m 앞이 막혀있을 시
                float check_dist = 1.5f;
                float ahead_x = loc.x + steer_x * check_dist;
                float ahead_z = loc.z + steer_z * check_dist;
                if (map.IsPathBlocked(loc.x, loc.z, ahead_x, ahead_z)) {
                    // 수직 벡터 계산하여 측면 회피 조향 주입
                    float perp_x = -steer_z;
                    float perp_z = steer_x;

                    // 좌/우 빈 공간 체크
                    float left_x = loc.x + perp_x * 1.2f;
                    float left_z = loc.z + perp_z * 1.2f;
                    float right_x = loc.x - perp_x * 1.2f;
                    float right_z = loc.z - perp_z * 1.2f;

                    bool left_blocked = map.IsPathBlocked(loc.x, loc.z, left_x, left_z);
                    bool right_blocked = map.IsPathBlocked(loc.x, loc.z, right_x, right_z);

                    float avoid_x = 0.0f;
                    float avoid_z = 0.0f;
                    if (!left_blocked) {
                        avoid_x = perp_x;
                        avoid_z = perp_z;
                    } else if (!right_blocked) {
                        avoid_x = -perp_x;
                        avoid_z = -perp_z;
                    } else {
                        avoid_x = perp_x;
                        avoid_z = perp_z;
                    }

                    // 회피 가중치 적용 (0.7f)
                    steer_x += avoid_x * 0.7f;
                    steer_z += avoid_z * 0.7f;

                    float steer_len = std::sqrt(steer_x * steer_x + steer_z * steer_z);
                    if (steer_len > 0.001f) {
                        steer_x /= steer_len;
                        steer_z /= steer_len;
                    }
                }

                vel.dir_x = steer_x;
                vel.dir_z = steer_z;

                float move_step = vel.speed * dt;
                loc.x += vel.dir_x * move_step;
                loc.z += vel.dir_z * move_step;

                grid.UpdateEntityPosition(entity, loc.x, loc.z, reg);
            }
            // 2. A* 웨이포인트 경로 이동 모드일 때
            else {
                if (path.current_waypoint_index >= path.waypoints.size()) {
                    // 더 이상 갈 노드가 없음 -> 목적지 도착
                    loc.x = job.target_x;
                    loc.y = job.target_y;
                    loc.z = job.target_z;

                    grid.UpdateEntityPosition(entity, loc.x, loc.z, reg);

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

                    bool is_sentient = true;
                    if (reg.all_of<SentienceComp>(entity)) {
                        is_sentient = reg.get<SentienceComp>(entity).is_sentient;
                    }
                    std::string verb = is_sentient ? "작업" : "행동";
                    
                    std::cout << "🏁 [목적지 도착 - A* Path] " << identity.display_name 
                              << "이(가) 목적지 [" << loc.location_name << "]에 도착하여 " << verb << "을(를) 시작합니다." << std::endl;
                    std::cout << "🔄 [Toil Transition] " << identity.display_name 
                              << ": Moving ➔ Working (A* 도달 완료, Job ID: " << job.job_id 
                              << ", Intent: " << job.intent << ")" << std::endl;
                    return;
                }

                const auto& target = path.waypoints[path.current_waypoint_index];

                float dx = target.x - loc.x;
                float dz = target.z - loc.z;
                float dist = std::sqrt(dx * dx + dz * dz);

                if (dist < 0.1f) {
                    path.current_waypoint_index++;
                    return;
                }

                vel.dir_x = dx / dist;
                vel.dir_z = dz / dist;

                float move_step = vel.speed * dt;

                if (move_step >= dist) {
                    loc.x = target.x;
                    loc.z = target.z;
                    path.current_waypoint_index++;
                } else {
                    loc.x += vel.dir_x * move_step;
                    loc.z += vel.dir_z * move_step;
                }
                grid.UpdateEntityPosition(entity, loc.x, loc.z, reg);
            }
        } else {
            vel.dir_x = 0.0f;
            vel.dir_z = 0.0f;
            vel.speed = 0.0f;
        }
    });
}
