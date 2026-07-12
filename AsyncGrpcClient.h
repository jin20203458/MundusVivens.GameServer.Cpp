#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <grpcpp/grpcpp.h>
#include <agrpc/asio_grpc.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "mundus_vivens.grpc.pb.h"
#include "MundusVivensClient.h"

namespace MundusVivens {

class AsyncGrpcClient {
public:
    using TickCallback = std::function<void(bool success, const std::string& message, const std::vector<uint32_t>& busy_agent_ids, const std::vector<RelationshipDelta>& relationship_deltas)>;
    using DialogueCallback = std::function<void(bool success, const DialogueResult& result)>;
    using StatusCallback = std::function<void(bool success, int32_t updated_count, const std::string& message)>;
    using StartDialogueCallback = std::function<void(bool success, uint64_t session_id, const std::string& greeting, const std::string& message)>;
    using SendPlayerMessageCallback = std::function<void(bool success, const std::string& reply)>;
    using EndDialogueCallback = std::function<void(bool success, const std::string& summary)>;
    using InjectBeliefCallback = std::function<void(bool success, const std::string& message)>;
    using GetAgentStatusCallback = std::function<void(bool success, const AgentStatus& status)>;

    // 생성자에서 채널, GrpcContext, io_context를 모두 공유받음
    AsyncGrpcClient(std::shared_ptr<grpc::Channel> channel,
                    agrpc::GrpcContext& grpc_context);
    ~AsyncGrpcClient();

    void ProcessWorldTickAsync(int32_t tick, TickCallback on_complete);
    void TriggerDialogueAsync(std::vector<uint32_t> participant_ids, DialogueCallback on_complete);
    void BatchUpdateStatusAsync(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete);

    // 플레이어 상호작용 관련 비동기 gRPC
    void StartPlayerDialogueAsync(std::string player_id, uint32_t npc_id, StartDialogueCallback on_complete);
    void SendPlayerMessageAsync(uint64_t session_id, std::string message, SendPlayerMessageCallback on_complete);
    void EndPlayerDialogueAsync(uint64_t session_id, EndDialogueCallback on_complete);

    //  비동기 믿음(소문) 주입
    void InjectBeliefAsync(uint32_t target_agent_id, uint32_t subject_id, std::string content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, InjectBeliefCallback on_complete);

    //  Job 및 Interrupt 관리 비동기 RPC
    using PendingJobsCallback = std::function<void(bool success, const std::vector<MundusVivensClient::JobPayload>& jobs)>;
    using ReportJobStatusCallback = std::function<void(bool success, bool has_new_job, const MundusVivensClient::JobPayload& new_job, const std::string& message)>;

    void GetPendingJobsAsync(int32_t current_tick, PendingJobsCallback on_complete);
    void ReportJobStatusAsync(uint32_t npc_id, uint64_t job_id, int32_t status, mundusvivens::InterruptReason reason_code, const std::string& detailed_context, int32_t current_tick, ReportJobStatusCallback on_complete);

    // 🆕 4단계: 충동-억제 기반 선제공격 및 피격 알림 인터페이스
    using ThreatDetectedCallback = std::function<void(bool success, int action_code)>;
    using ReportCombatEventCallback = std::function<void(bool success)>;

    void ThreatDetectedAsync(uint32_t agent_id, uint32_t target_agent_id, int32_t aggro_score, ThreatDetectedCallback on_complete);
    void ReportCombatEventAsync(uint32_t attacker_id, uint32_t victim_id, float damage, std::string weapon, ReportCombatEventCallback on_complete);
    void GetAgentStatusAsync(uint32_t agent_id, GetAgentStatusCallback on_complete);


private:
    boost::asio::awaitable<void> DoProcessWorldTick(int32_t tick, TickCallback on_complete);
    boost::asio::awaitable<void> DoTriggerDialogue(std::vector<uint32_t> participant_ids, DialogueCallback on_complete);
    boost::asio::awaitable<void> DoBatchUpdateStatus(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete);
    boost::asio::awaitable<void> DoStartPlayerDialogue(std::string player_id, uint32_t npc_id, StartDialogueCallback on_complete);
    boost::asio::awaitable<void> DoSendPlayerMessage(uint64_t session_id, std::string message, SendPlayerMessageCallback on_complete);
    boost::asio::awaitable<void> DoEndPlayerDialogue(uint64_t session_id, EndDialogueCallback on_complete);
    boost::asio::awaitable<void> DoInjectBelief(uint32_t target_agent_id, uint32_t subject_id, std::string content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, InjectBeliefCallback on_complete);
    boost::asio::awaitable<void> DoGetPendingJobs(int32_t current_tick, PendingJobsCallback on_complete);
    boost::asio::awaitable<void> DoReportJobStatus(uint32_t npc_id, uint64_t job_id, int32_t status, mundusvivens::InterruptReason reason_code, std::string detailed_context, int32_t current_tick, ReportJobStatusCallback on_complete);
    
    // 🆕 4단계 내부 코루틴 선언
    boost::asio::awaitable<void> DoThreatDetected(uint32_t agent_id, uint32_t target_agent_id, int32_t aggro_score, ThreatDetectedCallback on_complete);
    boost::asio::awaitable<void> DoReportCombatEvent(uint32_t attacker_id, uint32_t victim_id, float damage, std::string weapon, ReportCombatEventCallback on_complete);
    boost::asio::awaitable<void> DoGetAgentStatus(uint32_t agent_id, GetAgentStatusCallback on_complete);

    std::unique_ptr<mundusvivens::MundusVivensGrpc::Stub> stub_;
    agrpc::GrpcContext& grpc_ctx_;
};

} // namespace MundusVivens
