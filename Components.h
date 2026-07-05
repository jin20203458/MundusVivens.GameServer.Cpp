#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <entt/entt.hpp>
#include "GridMap.h"

// NPC 식별 정보 (NpcIds, NpcNames 대체)
struct IdentityComp {
    uint32_t npc_id = 0;
    std::string display_name;
};

// 성격 정보 (외향성)
struct PersonalityComp {
    float extroversion = 0.5f;
};

// 관계 엔트리 (친밀도 Liking, 신뢰도 Trust)
struct RelationshipEntry {
    int liking = 0;
    int trust = 50;
};

// 관계망 정보 캐시
struct RelationshipCacheComp {
    std::unordered_map<uint32_t, RelationshipEntry> relationships; // 상대 NPC ID -> 관계 정보
};

// 위치 정보 (CurrentLocations 대체)
struct LocationComp {
    uint32_t zone_id = 0;
    std::string location_name;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 감정 상태 (CurrentEmotions, BaseEmotions, EmotionDecayTicks 통합)
struct EmotionComp {
    std::string current_emotion;
    std::string base_emotion;
    uint8_t current_emotion_id = 0;  // 🆕 고속 비교 및 쇠퇴 룩업용
    uint8_t base_emotion_id = 0;
    int decay_ticks_remaining = 0;
};

// 활동 상태 (CurrentActivities 대체)
struct ActivityComp {
    std::string current_activity;
};

// 쿨다운 및 사회적 에너지 정보
struct CooldownComp {
    std::unordered_map<uint32_t, int32_t> cooldown_per_target; // 상대별 쿨다운 버퍼 (target_npc_id -> 틱)
    int32_t last_initiative_tick = 0;                          // 마지막으로 주도를 시도한 틱 (스팸 방지)
    int32_t social_energy = 100;                               // 현재 사회적 에너지 (외향성에 따라 다름)
    int32_t max_social_energy = 100;                           // 최대 사회적 에너지 (외향성에 따라 다름)
    int32_t cognitive_refractory_until = 0;                    // 인지적 불응기 해제 틱 (대화 직후 다른 자극 무시)
};


// 🆕 이동 속도 및 방향 벡터 (렌더링 동기화용)
struct VelocityComp {
    float speed = 2.0f;     // m/s
    float dir_x = 0.0f;
    float dir_z = 0.0f;
};

// 🆕 길찾기 결과 경로
struct PathfindingComp {
    std::vector<GridVector2> waypoints;   // 남은 경로의 타일 좌표들
    size_t current_waypoint_index = 0;    // 현재 목표 웨이포인트 인덱스
};


// Axis 2: Job 컴포넌트 (C# 대뇌가 할당한 고차원 의도)
struct JobComp {
    uint64_t job_id = 0;
    std::string target_location;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
    std::string intent;
    uint32_t target_agent_id = 0;
    int32_t priority = 0;
    bool is_active = false;
};

// Axis 2: Toil 컴포넌트 (C++ 척수가 실행하는 마이크로 상태)
enum class ToilState {
    Idle,
    Moving,
    Working,
    Interrupted
};

struct ToilComp {
    ToilState state = ToilState::Idle;
    std::string current_action;
    int32_t duration_ticks = 0;
};

// 🆕 생체 욕구 컴포넌트
struct NeedsComp {
    float hunger = 100.0f;
    float fatigue = 100.0f;
    bool is_resolving_survival = false;
    std::string current_survival_type; // "hunger", "fatigue", 또는 ""
    entt::entity occupied_furniture = entt::null; // 점유 중인 가구/사물 엔티티
};

// 🆕 사물 상호작용 종류
enum class AffordanceType {
    Sit,
    Sleep,
    Eat,
    Drink,
    Pray
};

// 🆕 사물 상호작용 컴포넌트
struct AffordanceComp {
    AffordanceType type;
    entt::entity occupied_by = entt::null;
};

// 네트워크 동기화 캐시 
struct LastSyncedComp {
    std::string location;
    std::string emotion;
    std::string activity;
};

// 🆕 대기 멈춤의 원인 열거형
enum class BusyReason {
    Dialogue,
    Reflection,
    ScheduleWait
};

// 대화 중이거나 성찰/계획대기 중인 바쁜 NPC를 나타내는 상태 컴포넌트
struct BusyTag {
    BusyReason reason = BusyReason::Dialogue;
    float anim_timer = 0.0f;
};

// 접속한 플레이어를 나타내는 태그 컴포넌트
struct PlayerTag {
    uint32_t session_index = 0;  // TcpServer의 세션 인덱스 (패킷 전송용)
};

// 플레이어-NPC 대화 상태를 관리하는 컴포넌트
struct PlayerDialogueComp {
    uint64_t session_id = 0;       // C# AI 서버가 발급한 플레이어 대화 세션 ID
    entt::entity npc_entity = entt::null; // 대화 상대 NPC 엔티티 핸들
};

// 역방향 인덱스 리소스 (EnTT registry.ctx()에 저장용)
struct EntityIndex {
    std::unordered_map<uint32_t, entt::entity>    by_npc_id;       // NPC ID -> entity
    std::unordered_map<uint32_t, entt::entity>    by_session_index; // 세션 번호 -> entity
};

// 🆕 동적 에이전트 ID 맵퍼 구조체
struct AgentIdMapper {
    std::unordered_map<std::string, uint32_t> string_to_numeric;
    std::unordered_map<uint32_t, std::string> numeric_to_string;
};

// 🆕 고속 캐시 프렌들리 감정 레지스트리 구조체
struct EmotionRegistry {
    std::unordered_map<std::string, uint8_t> name_to_id;
    std::vector<int32_t> decay_ticks_table; // 감정 ID -> 지속 틱수 (Flat Array, O(1))
};

