#include "Systems.h"
#include "Components.h"

// 🆕 20Hz 메인 루프에서 에이전트들의 로컬 행동 트리(BT)를 업데이트하는 시스템
void SystemBehaviorTree(entt::registry& reg) {
    auto view = reg.view<BehaviorTreeComp>();
    view.each([&](entt::entity entity, BehaviorTreeComp& bt) {
        if (bt.root_node) {
            // NPC가 바쁜 상태(BusyTag - 대화 중, 성찰 중 등)인 경우 의사결정 트리 처리를 일시 정지
            if (reg.all_of<BusyTag>(entity)) {
                return;
            }
            
            // 행동 트리 틱 실행
            bt.root_node->Tick(reg, entity);
        }
    });
}
