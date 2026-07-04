#pragma once

#include <string>
#include <vector>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "mundus_vivens.grpc.pb.h"

namespace MundusVivens {

struct DialogueLine {
    uint32_t speaker_id;
    std::string speaker_name;
    std::string text;
};

struct AgentEmotionUpdate {
    uint32_t agent_id;
    std::string new_emotion;
    int intensity = 0;
};

struct JobPayload {
    uint32_t npc_id;
    uint64_t job_id;
    std::string target_location;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float target_z = 0.0f;
    std::string intent;
    uint32_t target_agent_id = 0;
    int32_t priority = 0;
};

struct DialogueResult {
    uint64_t task_id;
    bool is_queued = false;
    bool is_completed = false;
    bool completed_immediately = false;
    std::string dialogue_summary;
    std::vector<std::string> dialogue_lines;
    std::vector<DialogueLine> structured_lines;
    std::vector<AgentEmotionUpdate> emotion_updates; 
    std::vector<JobPayload> next_jobs; 
    bool has_error = false; 
};

struct RelationshipSnapshot {
    uint32_t target_agent_id;
    int32_t liking;
    int32_t trust;
};

struct RelationshipDelta {
    uint32_t from_agent_id;
    uint32_t to_agent_id;
    int32_t liking;
    int32_t trust;
};

struct InitialAgentState {
    uint32_t agent_id;
    std::string name;
    std::string location;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string emotion;
    std::string activity;
    float extroversion = 0.5f;
    std::vector<RelationshipSnapshot> relationships;
    std::string string_id;
};

struct LocationData {
    std::string name;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 🆕 월드 부트스트랩 데이터 구조체
struct WorldBootstrapData {
    std::vector<LocationData> locations;
    std::vector<InitialAgentState> agents;
};

struct AgentStatus {
    std::string name;
    std::string location;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string emotion;
    std::string activity;
    std::vector<std::string> memories;
};

// 🆕 배치 상태 업데이트용 구조체
struct AgentStatusUpdate {
    uint32_t agent_id;
    std::string location;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::string emotion;
    std::string activity;
};

class MundusVivensClient {
public:
    // C# AI 서버의 공유 gRPC 채널을 전달받아 클라이언트를 생성합니다.
    MundusVivensClient(std::shared_ptr<grpc::Channel> channel);

    // NPC 간 대화를 트리거합니다.
    DialogueResult TriggerDialogue(uint32_t agent_id_a, uint32_t agent_id_b);

    // 특정 에이전트의 실시간 상태(위치, 감정, 에피소드 기억 요약 등)를 조회합니다.
    AgentStatus GetAgentStatus(uint32_t agent_id);

    // 특정 에이전트에게 믿음(소문)을 강제로 주입합니다.
    bool InjectBelief(uint32_t target_agent_id, uint32_t subject_id, const std::string& content, mundusvivens::ProtoBeliefType belief_type, std::string& out_message);



    // 🆕 에이전트 상태 배치 업데이트 RPC (Phase 5-2 신규)
    bool BatchUpdateAgentStatus(const std::vector<AgentStatusUpdate>& updates, int32_t& out_updated_count, std::string& out_message);

    // C++ 게임 서버의 틱(시간 흐름) 진행을 C# AI 서버에 알립니다.
    bool ProcessWorldTick(int32_t tick_number, std::string& out_message, std::vector<uint32_t>& out_busy_agent_ids);

    // 🆕 월드 부트스트랩 데이터 조회
    WorldBootstrapData GetWorldBootstrap();



    // 🚀 Axis 2: Job 및 Interrupt 관리 RPC
    using JobPayload = MundusVivens::JobPayload;

    std::vector<JobPayload> GetPendingJobs(int32_t current_tick);
    bool ReportJobStatus(uint32_t npc_id, uint64_t job_id, int32_t status, mundusvivens::InterruptReason reason_code, const std::string& detailed_context, int32_t current_tick, JobPayload& out_new_job);

private:
    std::unique_ptr<mundusvivens::MundusVivensGrpc::Stub> stub_;
};

} // namespace MundusVivens
