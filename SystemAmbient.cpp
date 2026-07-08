#include "Systems.h"
#include "Components.h"
#include <iostream>

//  NPC 대기 중 상태 조율 시스템
void SystemBusyAmbient(entt::registry& reg, float deltaTime) {
    auto view = reg.view<BusyTag, ActivityComp, ToilComp, VelocityComp, IdentityComp>();
    view.each([&](entt::entity entity, BusyTag& busy, ActivityComp& act, ToilComp& toil, VelocityComp& vel, IdentityComp& identity) {
        busy.anim_timer += deltaTime;

        // Dialogue/Reflection 상태일 때는 이동 제어
        if (busy.reason == BusyReason::Dialogue || busy.reason == BusyReason::Reflection) {
            vel.dir_x = 0.0f;
            vel.dir_z = 0.0f;
            vel.speed = 0.0f;
        }

        switch (busy.reason) {
            case BusyReason::Dialogue: {
                act.current_activity = "대화 중";
                toil.current_action = "Dialogue_Listening";
                
                // C++ 대화 상태가 셋업되어 있으면 current_action 업데이트
                if (toil.state == ToilState::Interrupted) {
                    toil.current_action = "Dialogue_Talking";
                }
                break;
            }
            case BusyReason::Reflection: {
                act.current_activity = "성찰 중";
                toil.current_action = "Thinking";
                if (busy.anim_timer > 10.0f) {
                    std::cout << "🧠 [성찰 진행] " << identity.display_name << "이(가) 하루를 성찰하며 조용히 생각에 잠겨 있습니다." << std::endl;
                    busy.anim_timer = 0.0f;
                }
                break;
            }
            case BusyReason::ScheduleWait: {
                act.current_activity = "스케줄 대기";
                toil.current_action = "WanderingAround"; // 클라이언트가 이 문자열을 받아 제자리 서성임 애니메이션 재생
                
                if (busy.anim_timer > 8.0f) {
                    std::cout << "☕ [스케줄 지연] " << identity.display_name << "이(가) 오늘 일정을 기다리며 집안을 서성이고 있습니다." << std::endl;
                    busy.anim_timer = 0.0f;
                }
                break;
            }
        }
    });
}
