#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <entt/entt.hpp>

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
};

// 감정 상태 (CurrentEmotions, BaseEmotions, EmotionDecayTicks 통합)
struct EmotionComp {
    std::string current_emotion;
    std::string base_emotion;
    int decay_ticks_remaining = 0;
};

// 활동 상태 (CurrentActivities 대체)
struct ActivityComp {
    std::string current_activity;
};

// 쿨다운 및 대화 횟수 정보 (GlobalDialogueCooldowns, DailyDialogueCounts 통합)
struct CooldownComp {
    int global_cooldown_until = 0;
    int daily_dialogue_count = 0;
};


// Axis 2: Job 컴포넌트 (C# 대뇌가 할당한 고차원 의도)
struct JobComp {
    uint64_t job_id = 0;
    std::string target_location;
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

// 네트워크 동기화 캐시 
struct LastSyncedComp {
    std::string location;
    std::string emotion;
    std::string activity;
};

// 대화 중인 바쁜 NPC를 나타내는 태그 컴포넌트
struct BusyTag {};

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

