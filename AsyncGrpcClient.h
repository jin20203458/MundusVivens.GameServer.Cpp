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
    using TickCallback = std::function<void(bool success, const std::string& message, const std::vector<std::string>& busy_agent_ids)>;
    using DialogueCallback = std::function<void(bool success, const DialogueResult& result)>;
    using StatusCallback = std::function<void(bool success, int32_t updated_count, const std::string& message)>;
    using StartDialogueCallback = std::function<void(bool success, const std::string& session_id, const std::string& greeting, const std::string& message)>;
    using SendMessageCallback = std::function<void(bool success, const std::string& reply)>;
    using EndDialogueCallback = std::function<void(bool success, const std::string& summary)>;

    // 생성자에서 채널, GrpcContext, io_context를 모두 공유받음
    AsyncGrpcClient(std::shared_ptr<grpc::Channel> channel,
                    agrpc::GrpcContext& grpc_context,
                    boost::asio::io_context& io_context);
    ~AsyncGrpcClient();

    void ProcessWorldTickAsync(int32_t tick, TickCallback on_complete);
    void TriggerDialogueAsync(const std::string& agent_id_a, const std::string& agent_id_b, DialogueCallback on_complete);
    void PollDialogueResultAsync(const std::string& task_id, DialogueCallback on_complete);
    void BatchUpdateStatusAsync(const std::vector<AgentStatusUpdate>& updates, StatusCallback on_complete);

    // 플레이어 상호작용 관련 비동기 gRPC
    void StartPlayerDialogueAsync(const std::string& player_id, const std::string& npc_id, StartDialogueCallback on_complete);
    void SendPlayerMessageAsync(const std::string& session_id, const std::string& message, SendMessageCallback on_complete);
    void EndPlayerDialogueAsync(const std::string& session_id, EndDialogueCallback on_complete);

    // ❌ DrainCompletedResults() 함수 삭제 (스레드 병합 및 자동 이벤트 디스패치로 불필요)

private:
    boost::asio::awaitable<void> DoProcessWorldTick(int32_t tick, TickCallback on_complete);
    boost::asio::awaitable<void> DoTriggerDialogue(std::string agent_id_a, std::string agent_id_b, DialogueCallback on_complete);
    boost::asio::awaitable<void> DoPollDialogueResult(std::string task_id, DialogueCallback on_complete);
    boost::asio::awaitable<void> DoBatchUpdateStatus(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete);
    boost::asio::awaitable<void> DoStartPlayerDialogue(std::string player_id, std::string npc_id, StartDialogueCallback on_complete);
    boost::asio::awaitable<void> DoSendPlayerMessage(std::string session_id, std::string message, SendMessageCallback on_complete);
    boost::asio::awaitable<void> DoEndPlayerDialogue(std::string session_id, EndDialogueCallback on_complete);

    std::unique_ptr<mundusvivens::MundusVivensGrpc::Stub> stub_;
    agrpc::GrpcContext& grpc_ctx_;
    boost::asio::io_context& io_ctx_;
};

} // namespace MundusVivens
