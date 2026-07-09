#include "Systems.h"
#include "Components.h"
#include <iostream>
#include <cmath>

// A* 길찾기 적용 시스템
void SystemPathfinding(entt::registry& reg, const GridMap& map) {
    auto view = reg.view<JobComp, ToilComp, LocationComp>();
    view.each([&](entt::entity entity, JobComp& job, ToilComp& toil, LocationComp& loc) {
        if (toil.state == ToilState::Moving) {
            auto& pathfinding = reg.get_or_emplace<PathfindingComp>(entity);

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
                    auto& vel = reg.get_or_emplace<VelocityComp>(entity);
                    float move_speed = 2.0f;
                    if (reg.ctx().contains<SimulationSettings>()) {
                        move_speed = reg.ctx().get<SimulationSettings>().npc_speed;
                    }
                    vel.speed = move_speed;
                    
                    if (auto* ident = reg.try_get<IdentityComp>(entity)) {
                        std::cout << "🧭 [경로 생성] " << ident->display_name << "이(가) [" << job.target_location << "] (" << target_x << ", " << target_z << ")로의 경로를 탐색하여 " << pathfinding.waypoints.size() << "개의 노드를 찾았습니다." << std::endl;
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
void SystemMovement(entt::registry& reg, LocationRegistry& grid, int tick) {
    constexpr float dt = 0.05f; // 20Hz 기준 dt

    auto view = reg.view<LocationComp, ToilComp, PathfindingComp, VelocityComp, JobComp, IdentityComp>();
    view.each([&](entt::entity entity, LocationComp& loc, ToilComp& toil, PathfindingComp& path, VelocityComp& vel, JobComp& job, IdentityComp& identity) {
        if (toil.state == ToilState::Moving) {
            if (path.current_waypoint_index >= path.waypoints.size()) {
                // 더 이상 갈 노드가 없음 -> 목적지 도착
                loc.x = job.target_x;
                loc.y = job.target_y;
                loc.z = job.target_z;

                // 위치 등록기를 통해 위치 및 소속 구역 갱신
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
                
                std::cout << "🏁 [목적지 도착] " << identity.display_name 
                          << "이(가) 목적지 [" << loc.location_name << "]에 도착하여 작업을 시작합니다." << std::endl;
                std::cout << "🔄 [Toil Transition] " << identity.display_name 
                          << ": Moving ➔ Working (물리 도달 완료, Job ID: " << job.job_id 
                          << ", Intent: " << job.intent << ")" << std::endl;
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
            // 이동 후 실시간으로 위치 등록기 갱신
            grid.UpdateEntityPosition(entity, loc.x, loc.z, reg);
        } else {
            // Moving이 아닌 경우 속도 리셋
            vel.dir_x = 0.0f;
            vel.dir_z = 0.0f;
            vel.speed = 0.0f;
        }
    });
}
