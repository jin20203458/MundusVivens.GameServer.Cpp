#include "AsyncGrpcClient.h"
#include <grpcpp/create_channel.h>
#include <agrpc/client_rpc.hpp>
#include <iostream>
#include <chrono>

namespace MundusVivens {

AsyncGrpcClient::AsyncGrpcClient(std::shared_ptr<grpc::Channel> channel,
                                 agrpc::GrpcContext& grpc_context)
    : stub_(mundusvivens::MundusVivensGrpc::NewStub(channel)),
      grpc_ctx_(grpc_context) {
}

AsyncGrpcClient::~AsyncGrpcClient() {
}

// -------------------------------------------------------------
// Public Interface (Shorthand co_spawns)
// -------------------------------------------------------------

void AsyncGrpcClient::ProcessWorldTickAsync(int32_t tick, TickCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoProcessWorldTick(tick, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::TriggerDialogueAsync(std::vector<uint32_t> participant_ids, DialogueCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoTriggerDialogue(std::move(participant_ids), std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::BatchUpdateStatusAsync(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoBatchUpdateStatus(std::move(updates), std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::StartPlayerDialogueAsync(std::string player_id, uint32_t npc_id, StartDialogueCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoStartPlayerDialogue(std::move(player_id), npc_id, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::SendPlayerMessageAsync(uint64_t session_id, std::string message, SendPlayerMessageCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoSendPlayerMessage(session_id, std::move(message), std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::EndPlayerDialogueAsync(uint64_t session_id, EndDialogueCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoEndPlayerDialogue(session_id, std::move(on_complete)),
        boost::asio::detached);
}

void AsyncGrpcClient::InjectBeliefAsync(uint32_t target_agent_id, uint32_t subject_id, std::string content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, InjectBeliefCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoInjectBelief(target_agent_id, subject_id, std::move(content), belief_type, source_agent_id, std::move(on_complete)),
        boost::asio::detached);
}
// -------------------------------------------------------------
// Private Coroutine Implementations (Safe parameter lifetime)
// -------------------------------------------------------------

boost::asio::awaitable<void> AsyncGrpcClient::DoProcessWorldTick(int32_t tick, TickCallback on_complete) {
    try {
        // agrpc::ClientRPC 를 사용하여 비동기 gRPC 요청을 준비하고 전송합니다
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
            std::vector<uint32_t> busy_ids(response.busy_agent_ids().begin(), response.busy_agent_ids().end());
            std::vector<RelationshipDelta> relationship_deltas;
            for (int i = 0; i < response.relationship_deltas_size(); ++i) {
                const auto& proto_delta = response.relationship_deltas(i);
                RelationshipDelta delta;
                delta.from_agent_id = proto_delta.from_agent_id();
                delta.to_agent_id = proto_delta.to_agent_id();
                delta.liking = proto_delta.liking();
                delta.trust = proto_delta.trust();
                relationship_deltas.push_back(delta);
            }
            on_complete(response.success(), response.message(), busy_ids, relationship_deltas);
        } else {
            on_complete(false, "gRPC error: " + status.error_message(), {}, {});
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in ProcessWorldTickAsync] " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "❌ [Unknown Exception in ProcessWorldTickAsync]" << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoTriggerDialogue(std::vector<uint32_t> participant_ids, DialogueCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncTriggerDialogue>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));

        mundusvivens::TriggerDialogueRequest request;
        if (participant_ids.size() >= 2) {
            request.set_agent_id_a(participant_ids[0]);
            request.set_agent_id_b(participant_ids[1]);
        }
        for (uint32_t pid : participant_ids) {
            request.add_participant_ids(pid);
        }
        mundusvivens::TriggerDialogueResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        DialogueResult result;
        if (status.ok()) {
            result.task_id = response.task_id();
            result.dialogue_summary = response.dialogue_summary();
            result.dialogue_lines.assign(response.dialogue_lines().begin(), response.dialogue_lines().end());
            result.structured_lines.reserve(response.structured_lines_size());
            for (int i = 0; i < response.structured_lines_size(); ++i) {
                const auto& proto_line = response.structured_lines(i);
                result.structured_lines.push_back(DialogueLine{
                    proto_line.speaker_id(),
                    proto_line.speaker_name(),
                    proto_line.text()
                });
            }
            result.emotion_updates.reserve(response.emotion_updates_size());
            for (int i = 0; i < response.emotion_updates_size(); ++i) {
                const auto& proto_update = response.emotion_updates(i);
                result.emotion_updates.push_back(AgentEmotionUpdate{
                    proto_update.agent_id(),
                    proto_update.new_emotion(),
                    static_cast<int>(proto_update.intensity()),
                    static_cast<uint8_t>(proto_update.category())
                });
            }
            result.next_jobs.reserve(response.next_jobs_size());
            for (int i = 0; i < response.next_jobs_size(); ++i) {
                const auto& proto_job = response.next_jobs(i);
                MundusVivensClient::JobPayload job;
                job.npc_id = proto_job.npc_id();
                job.job_id = proto_job.job_id();
                job.target_location = proto_job.target_location().name();
                job.target_x = proto_job.target_location().position().x();
                job.target_y = proto_job.target_location().position().y();
                job.target_z = proto_job.target_location().position().z();
                job.intent = proto_job.intent();
                job.target_agent_id = proto_job.target_agent_id();
                job.priority = proto_job.priority();
                job.category = static_cast<uint8_t>(proto_job.category());
                result.next_jobs.push_back(job);
            }
            result.keywords.reserve(response.keywords_size());
            for (int i = 0; i < response.keywords_size(); ++i) {
                result.keywords.push_back(response.keywords(i));
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

boost::asio::awaitable<void> AsyncGrpcClient::DoBatchUpdateStatus(std::vector<AgentStatusUpdate> updates, StatusCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncBatchUpdateAgentStatus>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::BatchUpdateAgentStatusRequest request;
        for (const auto& update : updates) {
            auto* agent_req = request.add_agents();
            agent_req->set_agent_id(update.agent_id);
            auto* loc = agent_req->mutable_location();
            loc->set_name(update.location);
            auto* pos = loc->mutable_position();
            pos->set_x(update.x);
            pos->set_y(update.y);
            pos->set_z(update.z);
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

boost::asio::awaitable<void> AsyncGrpcClient::DoStartPlayerDialogue(std::string player_id, uint32_t npc_id, StartDialogueCallback on_complete) {
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
            on_complete(false, 0, "", "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in StartPlayerDialogueAsync] " << e.what() << std::endl;
    }
}

boost::asio::awaitable<void> AsyncGrpcClient::DoSendPlayerMessage(uint64_t session_id, std::string message, SendPlayerMessageCallback on_complete) {
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

boost::asio::awaitable<void> AsyncGrpcClient::DoEndPlayerDialogue(uint64_t session_id, EndDialogueCallback on_complete) {
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

boost::asio::awaitable<void> AsyncGrpcClient::DoInjectBelief(uint32_t target_agent_id, uint32_t subject_id, std::string content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, InjectBeliefCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncInjectBelief>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::InjectBeliefRequest request;
        request.set_target_agent_id(target_agent_id);
        request.set_subject_id(subject_id);
        request.set_content(content);
        request.set_belief_type(belief_type);
        request.set_source_agent_id(source_agent_id);
        mundusvivens::InjectBeliefResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            on_complete(response.success(), response.message());
        } else {
            on_complete(false, "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in InjectBeliefAsync] " << e.what() << std::endl;
    }
}

void AsyncGrpcClient::GetPendingJobsAsync(int32_t current_tick, PendingJobsCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoGetPendingJobs(current_tick, std::move(on_complete)),
        boost::asio::detached);
}

boost::asio::awaitable<void> AsyncGrpcClient::DoGetPendingJobs(int32_t current_tick, PendingJobsCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncGetPendingJobs>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::GetPendingJobsRequest request;
        request.set_current_tick(current_tick);
        mundusvivens::GetPendingJobsResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            std::vector<MundusVivensClient::JobPayload> jobs;
            jobs.reserve(response.jobs_size());
            for (int i = 0; i < response.jobs_size(); ++i) {
                const auto& proto_job = response.jobs(i);
                MundusVivensClient::JobPayload job;
                job.npc_id = proto_job.npc_id();
                job.job_id = proto_job.job_id();
                job.target_location = proto_job.target_location().name();
                job.target_x = proto_job.target_location().position().x();
                job.target_y = proto_job.target_location().position().y();
                job.target_z = proto_job.target_location().position().z();
                job.intent = proto_job.intent();
                job.target_agent_id = proto_job.target_agent_id();
                job.priority = proto_job.priority();
                job.category = static_cast<uint8_t>(proto_job.category());
                jobs.push_back(job);
            }
            on_complete(true, jobs);
        } else {
            on_complete(false, {});
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in GetPendingJobsAsync] " << e.what() << std::endl;
        on_complete(false, {});
    }
}

void AsyncGrpcClient::ReportJobStatusAsync(uint32_t npc_id, uint64_t job_id, int32_t status_val, mundusvivens::InterruptReason reason_code, const std::string& detailed_context, int32_t current_tick, ReportJobStatusCallback on_complete) {
    boost::asio::co_spawn(grpc_ctx_,
        DoReportJobStatus(npc_id, job_id, status_val, reason_code, detailed_context, current_tick, std::move(on_complete)),
        boost::asio::detached);
}

boost::asio::awaitable<void> AsyncGrpcClient::DoReportJobStatus(uint32_t npc_id, uint64_t job_id, int32_t status_val, mundusvivens::InterruptReason reason_code, std::string detailed_context, int32_t current_tick, ReportJobStatusCallback on_complete) {
    try {
        using RPC = agrpc::ClientRPC<&mundusvivens::MundusVivensGrpc::Stub::PrepareAsyncReportJobStatus>;
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

        mundusvivens::ReportJobStatusRequest request;
        request.set_npc_id(npc_id);
        request.set_job_id(job_id);
        request.set_status(static_cast<mundusvivens::ReportJobStatusRequest_JobStatus>(status_val));
        request.set_reason_code(reason_code);
        request.set_detailed_context(detailed_context);
        request.set_current_tick(current_tick);
        mundusvivens::ReportJobStatusResponse response;

        const grpc::Status status = co_await RPC::request(
            grpc_ctx_, *stub_, context, request, response, boost::asio::use_awaitable
        );

        if (status.ok()) {
            MundusVivensClient::JobPayload new_job{};
            bool has_new = response.has_new_job();
            if (has_new) {
                const auto& proto_job = response.new_job();
                new_job.npc_id = proto_job.npc_id();
                new_job.job_id = proto_job.job_id();
                new_job.target_location = proto_job.target_location().name();
                new_job.target_x = proto_job.target_location().position().x();
                new_job.target_y = proto_job.target_location().position().y();
                new_job.target_z = proto_job.target_location().position().z();
                new_job.intent = proto_job.intent();
                new_job.target_agent_id = proto_job.target_agent_id();
                new_job.priority = proto_job.priority();
                new_job.category = static_cast<uint8_t>(proto_job.category());
            }
            on_complete(response.success(), has_new, new_job, response.message());
        } else {
            on_complete(false, false, {}, "gRPC error: " + status.error_message());
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ [Exception in ReportJobStatusAsync] " << e.what() << std::endl;
        on_complete(false, false, {}, e.what());
    }
}

} // namespace MundusVivens
