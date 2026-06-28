#include "AsyncGrpcClient.h"
#include <grpcpp/create_channel.h>
#include <agrpc/client_rpc.hpp>
#include <iostream>
#include <chrono>

namespace MundusVivens {

AsyncGrpcClient::AsyncGrpcClient(std::shared_ptr<grpc::Channel> channel,
                                 agrpc::GrpcContext& grpc_context,
                                 boost::asio::io_context& io_context)
    : stub_(mundusvivens::MundusVivensGrpc::NewStub(channel)),
      grpc_ctx_(grpc_context),
      io_ctx_(io_context) {
}

AsyncGrpcClient::~AsyncGrpcClient() {
}

// -------------------------------------------------------------
// Public Interface (Shorthand co_spawns)
// -------------------------------------------------------------

void AsyncGrpcClient::ProcessWorldTickAsync(int32_t tick, TickCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoProcessWorldTick(tick, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::TriggerDialogueAsync(const std::string& agent_id_a, const std::string& agent_id_b, DialogueCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoTriggerDialogue(agent_id_a, agent_id_b, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::PollDialogueResultAsync(const std::string& task_id, DialogueCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoPollDialogueResult(task_id, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::BatchUpdateStatusAsync(const std::vector<AgentStatusUpdate>& updates, StatusCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoBatchUpdateStatus(updates, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::StartPlayerDialogueAsync(const std::string& player_id, const std::string& npc_id, StartDialogueCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoStartPlayerDialogue(player_id, npc_id, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::SendPlayerMessageAsync(const std::string& session_id, const std::string& message, SendMessageCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoSendPlayerMessage(session_id, message, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::EndPlayerDialogueAsync(const std::string& session_id, EndDialogueCallback on_complete) {
    boost::asio::co_spawn(io_ctx_,
        DoEndPlayerDialogue(session_id, std::move(on_complete)),
        boost::asio::detached);
}

// -------------------------------------------------------------
// Private Coroutine Implementations (Safe parameter lifetime)
// -------------------------------------------------------------

boost::asio::awaitable<void> AsyncGrpcClient::DoProcessWorldTick(int32_t tick, TickCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncProcessWorldTick>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::ProcessWorldTickRequest request;
        request.set_tick_number(tick);
        mundusvivens::ProcessWorldTickResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            std::vector<std::string> busy_ids;
            for (int i = 0; i < response.busy_agent_ids_size(); ++i) {
                busy_ids.push_back(response.busy_agent_ids(i));
            }
            on_complete(response.success(), response.message(), busy_ids);
        } else {
            on_complete(false, "gRPC error: " + status.error_message(), {});
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in ProcessWorldTickAsync] " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "❌ [Unknown Exception in ProcessWorldTickAsync]" << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoTriggerDialogue(std::string agent_id_a, std::string agent_id_b, DialogueCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncTriggerDialogue>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::TriggerDialogueRequest request;
        request.set_agent_id_a(agent_id_a);
        request.set_agent_id_b(agent_id_b);
        request.set_wait_for_completion(false);
        mundusvivens::TriggerDialogueResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        DialogueResult result;
        if (status.ok()) {
            result.task_id = response.task_id();
            result.is_queued = response.is_queued();
            result.completed_immediately = response.completed_immediately();
            result.dialogue_summary = response.dialogue_summary();
            result.is_completed = response.completed_immediately();
            for (int i = 0; i < response.dialogue_lines_size(); ++i) {
                result.dialogue_lines.push_back(response.dialogue_lines(i));
            }
            for (int i = 0; i < response.structured_lines_size(); ++i) {
                const auto& proto_line = response.structured_lines(i);
                DialogueLine line;
                line.speaker_id = proto_line.speaker_id();
                line.speaker_name = proto_line.speaker_name();
                line.text = proto_line.text();
                result.structured_lines.push_back(line);
            }
            for (int i = 0; i < response.emotion_updates_size(); ++i) {
                const auto& proto_update = response.emotion_updates(i);
                AgentEmotionUpdate update;
                update.agent_id = proto_update.agent_id();
                update.new_emotion = proto_update.new_emotion();
                result.emotion_updates.push_back(update);
            }
            on_complete(true, result);
        } else {
            result.has_error = true;
            result.dialogue_summary = "gRPC error: " + status.error_message();
            on_complete(false, result);
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in TriggerDialogueAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoPollDialogueResult(std::string task_id, DialogueCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncGetDialogueResult>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::GetDialogueResultRequest request;
        request.set_task_id(task_id);
        mundusvivens::GetDialogueResultResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        DialogueResult result;
        result.task_id = task_id;
        if (status.ok()) {
            result.is_completed = response.is_completed();
            result.dialogue_summary = response.dialogue_summary();
            if (result.is_completed) {
                for (int i = 0; i < response.lines_size(); ++i) {
                    const auto& proto_line = response.lines(i);
                    DialogueLine line;
                    line.speaker_id = proto_line.speaker_id();
                    line.speaker_name = proto_line.speaker_name();
                    line.text = proto_line.text();
                    result.structured_lines.push_back(line);
                    result.dialogue_lines.push_back(proto_line.speaker_name() + ": " + proto_line.text());
                }
                for (int i = 0; i < response.emotion_updates_size(); ++i) {
                    const auto& proto_update = response.emotion_updates(i);
                    AgentEmotionUpdate update;
                    update.agent_id = proto_update.agent_id();
                    update.new_emotion = proto_update.new_emotion();
                    result.emotion_updates.push_back(update);
                }
            }
            on_complete(true, result);
        } else {
            result.has_error = true;
            result.is_completed = false;
            result.dialogue_summary = "gRPC error: " + status.error_message();
            on_complete(false, result);
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in PollDialogueResultAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoBatchUpdateStatus(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncBatchUpdateAgentStatus>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::BatchUpdateAgentStatusRequest request;
        for (const auto& update : updates) {
            auto* agent_req = request.add_agents();
            agent_req->set_agent_id(update.agent_id);
            agent_req->set_location(update.location);
            agent_req->set_emotion(update.emotion);
            agent_req->set_activity(update.activity);
        }
        mundusvivens::BatchUpdateAgentStatusResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            on_complete(true, response.updated_count(), "배치 업데이트 완료");
        } else {
            on_complete(false, 0, "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in BatchUpdateStatusAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoStartPlayerDialogue(std::string player_id, std::string npc_id, StartDialogueCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncStartPlayerDialogue>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::StartPlayerDialogueRequest request;
        request.set_player_id(player_id);
        request.set_npc_id(npc_id);
        mundusvivens::StartPlayerDialogueResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            on_complete(response.success(), response.session_id(), response.greeting(), response.message());
        } else {
            on_complete(false, "", "", "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in StartPlayerDialogueAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoSendPlayerMessage(std::string session_id, std::string message, SendMessageCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncSendPlayerMessage>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::SendPlayerMessageRequest request;
        request.set_session_id(session_id);
        request.set_message(message);
        mundusvivens::SendPlayerMessageResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            on_complete(true, response.reply());
        } else {
            on_complete(false, "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in SendPlayerMessageAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoEndPlayerDialogue(std::string session_id, EndDialogueCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncEndPlayerDialogue>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::EndPlayerDialogueRequest request;
        request.set_session_id(session_id);
        mundusvivens::EndPlayerDialogueResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            on_complete(response.success(), response.summary());
        } else {
            on_complete(false, "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in EndPlayerDialogueAsync] " << e.what() << std::endl;
    }
}

} // namespace MundusVivens
