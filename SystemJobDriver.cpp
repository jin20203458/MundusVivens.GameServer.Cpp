#include "Systems.h"
#include "Components.h"
#include "GrpcResultQueue.h"
#include <iostream>
#include <cmath>

static const char* ToilStateToString(ToilState state) {
    switch (state) {
        case ToilState::Idle: return "Idle";
        case ToilState::Moving: return "Moving";
        case ToilState::Working: return "Working";
        case ToilState::Interrupted: return "Interrupted";
        default: return "Unknown";
    }
}

//  Job 및 Toil 상태 머신 제어 시스템
void SystemJobDriver(entt::registry& reg, LocationRegistry& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    auto view = reg.view<LocationComp, ActivityComp, IdentityComp>();

    view.each([&](entt::entity entity, LocationComp& loc, ActivityComp& act, IdentityComp& identity) {
        auto& job = reg.get_or_emplace<JobComp>(entity);
        auto& toil = reg.get_or_emplace<ToilComp>(entity);

        // 사망한 NPC는 고차원 스케줄 관리 중단
        if (reg.all_of<HealthComp>(entity) && reg.get<HealthComp>(entity).is_dead) {
            return;
        }

        //  만약 생체 위기 해결(로컬 BT) 중인 경우, 고차원 JobDriver 상태 머신은 중지
        if (reg.all_of<NeedsComp>(entity) && reg.get<NeedsComp>(entity).is_resolving_survival) {
            return;
        }

        // NPC가 대화중이거나 바쁘면 Toil 상태를 Interrupted로 전환하고 대기
        if (reg.all_of<BusyTag>(entity)) {
            if (toil.state != ToilState::Interrupted) {
                ToilState old_state = toil.state;
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
                                        j.category = static_cast<JobCategory>(new_job.category);
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
                std::cout << "🔄 [Toil Transition] " << identity.display_name << ": " 
                          << ToilStateToString(old_state) << " ➔ Interrupted (대화 시작)" << std::endl;
            }
            return;
        }

        // 바쁘지 않은데 상태가 Interrupted라면 Idle로 복귀
        if (toil.state == ToilState::Interrupted) {
            toil.state = ToilState::Idle;
            act.current_activity = "대기";
            std::cout << "🔄 [Toil Transition] " << identity.display_name 
                      << ": Interrupted ➔ Idle (대화 종료 복귀)" << std::endl;
        }

        if (!job.is_active) {
            toil.state = ToilState::Idle;
            act.current_activity = "대기";
            return;
        }

        switch (toil.state) {
            case ToilState::Idle: {
                if (loc.location_name != job.target_location) {
                    toil.state = ToilState::Moving;
                    act.current_activity = "이동 중: " + job.target_location;
                    std::cout << "🔄 [Toil Transition] " << identity.display_name 
                              << ": Idle ➔ Moving (Job ID: " << job.job_id 
                              << ", Target: " << job.target_location << ")" << std::endl;

                    // 원정 출발 전 생체 욕구 완충 (황무지 무역 대비)
                    float dx = job.target_x - loc.x;
                    float dz = job.target_z - loc.z;
                    float dist = std::sqrt(dx * dx + dz * dz);
                    if (dist > 150.0f) {
                        if (reg.all_of<NeedsComp>(entity)) {
                            auto& needs = reg.get<NeedsComp>(entity);
                            needs.hunger = 100.0f;
                            needs.fatigue = 100.0f;
                            std::cout << "🎒 [원정 준비] " << identity.display_name << "이(가) 장거리 여행(" 
                                      << job.target_location << ")에 대비해 식사와 휴식을 완전 충전하고 출발합니다." << std::endl;
                        }
                    }
                } else {
                    toil.state = ToilState::Working;
                    toil.duration_ticks = 3; // 기본적으로 3틱(30초) 동안 해당 활동 진행
                    act.current_activity = job.intent;
                    std::cout << "🔄 [Toil Transition] " << identity.display_name 
                              << ": Idle ➔ Working (즉시 도달, Job ID: " << job.job_id 
                              << ", Intent: " << job.intent << ")" << std::endl;
                }
                break;
            }
            case ToilState::Moving: {
                // [Smell #3 해결] 물리적으로 도달하지 않은 이동 중에는 이곳에서 강제 상태 전이를 하지 않습니다.
                // 오직 물리적으로 웨이포인트를 소모해 목적지에 완전히 닿았을 때(SystemMovement)만 Working으로 전이됩니다.
                break;
            }
            case ToilState::Working: {
                if (toil.duration_ticks > 0) {
                    toil.duration_ticks--;
                    std::cout << "🛠️ [Job 진행] " << identity.display_name << " 작업 중: [" << loc.location_name 
                              << "] (남은 틱: " << toil.duration_ticks << ")" << std::endl;
                }

                if (toil.duration_ticks <= 0) {
                    std::cout << "✅ [Job 완료] " << identity.display_name << "의 Job " << job.job_id << " 완료!" << std::endl;
                    job.is_active = false;
                    toil.state = ToilState::Idle;
                    act.current_activity = "대기";
                    std::cout << "🔄 [Toil Transition] " << identity.display_name 
                              << ": Working ➔ Idle (Job ID: " << job.job_id << " 완료)" << std::endl;
                    
                    reg.emplace_or_replace<BusyTag>(entity, BusyReason::ScheduleWait, 0.0f);

                    // C# 서버에 Job 완료 상태 보고
                    uint32_t npc_id = identity.npc_id;
                    uint64_t job_id = job.job_id;
                    client.ReportJobStatusAsync(npc_id, job_id, 0, mundusvivens::UNKNOWN_REASON, "스케줄 완료", current_tick,
                        [](bool success, bool has_new_job, const MundusVivens::MundusVivensClient::JobPayload& new_job, const std::string& message) {
                        });
                }
                break;
            }
            default:
                break;
        }
    });
}
