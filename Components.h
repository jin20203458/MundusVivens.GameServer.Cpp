#pragma once
#include <string>
#include <vector>
#include <cstdint>

// NPC 식별 정보 (NpcIds, NpcNames 대체)
struct IdentityComp {
    std::string npc_id;
    std::string display_name;
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

// 일일 스케줄 항목
struct ScheduleItem {
    int32_t start_hour = 0;
    int32_t end_hour = 0;
    std::string target_location;
    std::string activity;
};

// 스케줄 컴포넌트 (DailySchedules 대체)
struct ScheduleComp {
    std::vector<ScheduleItem> items;
};

// 네트워크 동기화 캐시 (LastSentLocations/LastSentEmotions/LastSentActivities 통합)
struct LastSyncedComp {
    std::string location;
    std::string emotion;
    std::string activity;
};

// 대화 중인 바쁜 NPC를 나타내는 태그 컴포넌트
struct BusyTag {};
