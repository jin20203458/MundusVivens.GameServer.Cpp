#include "AsyncGrpcClient.h"
#include <grpcpp/create_channel.h>
#include <iostream>

namespace MundusVivens {

AsyncGrpcClient::AsyncGrpcClient(const std::string& address) : shutting_down_(false) {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    stub_ = mundusvivens::MundusVivensGrpc::NewStub(channel);
    cq_worker_ = std::thread(&AsyncGrpcClient::WorkerLoop, this);
}

AsyncGrpcClient::~AsyncGrpcClient() {
    shutting_down_ = true;
    cq_.Shutdown();
    if (cq_worker_.joinable()) {
        cq_worker_.join();
    }
}

void AsyncGrpcClient::WorkerLoop() {
    void* tag = nullptr;
    bool ok = false;
    // CompletionQueue가 완전히 종료되고 모든 이벤트가 드레인될 때까지 루프
    while (cq_.Next(&tag, &ok)) {
        if (tag) {
            auto* rpc_tag = static_cast<AsyncRpcTag*>(tag);
            rpc_tag->HandleCompletion(ok);
            delete rpc_tag;
        }
    }
}

void AsyncGrpcClient::QueueCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    completed_callbacks_.push_back(std::move(cb));
}

void AsyncGrpcClient::DrainCompletedResults() {
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        callbacks.swap(completed_callbacks_);
    }
    for (const auto& cb : callbacks) {
        if (cb) {
            cb();
        }
    }
}

void AsyncGrpcClient::ProcessWorldTickAsync(int32_t tick, TickCallback on_complete) {
    auto* call = new RpcCall<mundusvivens::ProcessWorldTickResponse>();
    call->client = this;
    call->on_complete = [on_complete](bool ok, mundusvivens::ProcessWorldTickResponse& resp, const grpc::Status& status) {
        if (ok) {
            std::vector<std::string> busy_ids;
            for (int i = 0; i < resp.busy_agent_ids_size(); ++i) {
                busy_ids.push_back(resp.busy_agent_ids(i));
            }
            on_complete(resp.success(), resp.message(), busy_ids);
        } else {
            on_complete(false, "gRPC error: " + status.error_message(), {});
        }
    };

    mundusvivens::ProcessWorldTickRequest request;
    request.set_tick_number(tick);

    call->reader = stub_->PrepareAsyncProcessWorldTick(&call->context, request, &cq_);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, (void*)call);
}

void AsyncGrpcClient::TriggerDialogueAsync(const std::string& agent_id_a, const std::string& agent_id_b, DialogueCallback on_complete) {
    auto* call = new RpcCall<mundusvivens::TriggerDialogueResponse>();
    call->client = this;
    call->on_complete = [on_complete](bool ok, mundusvivens::TriggerDialogueResponse& resp, const grpc::Status& status) {
        DialogueResult result;
        if (ok) {
            result.task_id = resp.task_id();
            result.is_queued = resp.is_queued();
            result.completed_immediately = resp.completed_immediately();
            result.dialogue_summary = resp.dialogue_summary();
            result.is_completed = resp.completed_immediately();
            for (int i = 0; i < resp.dialogue_lines_size(); ++i) {
                result.dialogue_lines.push_back(resp.dialogue_lines(i));
            }
            for (int i = 0; i < resp.structured_lines_size(); ++i) {
                const auto& proto_line = resp.structured_lines(i);
                DialogueLine line;
                line.speaker_id = proto_line.speaker_id();
                line.speaker_name = proto_line.speaker_name();
                line.text = proto_line.text();
                result.structured_lines.push_back(line);
            }
            for (int i = 0; i < resp.emotion_updates_size(); ++i) {
                const auto& proto_update = resp.emotion_updates(i);
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
    };

    mundusvivens::TriggerDialogueRequest request;
    request.set_agent_id_a(agent_id_a);
    request.set_agent_id_b(agent_id_b);
    request.set_wait_for_completion(false);

    call->reader = stub_->PrepareAsyncTriggerDialogue(&call->context, request, &cq_);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, (void*)call);
}

void AsyncGrpcClient::PollDialogueResultAsync(const std::string& task_id, DialogueCallback on_complete) {
    auto* call = new RpcCall<mundusvivens::GetDialogueResultResponse>();
    call->client = this;
    call->on_complete = [task_id, on_complete](bool ok, mundusvivens::GetDialogueResultResponse& resp, const grpc::Status& status) {
        DialogueResult result;
        result.task_id = task_id;
        if (ok) {
            result.is_completed = resp.is_completed();
            result.dialogue_summary = resp.dialogue_summary();
            if (result.is_completed) {
                for (int i = 0; i < resp.lines_size(); ++i) {
                    const auto& proto_line = resp.lines(i);
                    DialogueLine line;
                    line.speaker_id = proto_line.speaker_id();
                    line.speaker_name = proto_line.speaker_name();
                    line.text = proto_line.text();
                    result.structured_lines.push_back(line);
                    result.dialogue_lines.push_back(proto_line.speaker_name() + ": " + proto_line.text());
                }
                for (int i = 0; i < resp.emotion_updates_size(); ++i) {
                    const auto& proto_update = resp.emotion_updates(i);
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
    };

    mundusvivens::GetDialogueResultRequest request;
    request.set_task_id(task_id);

    call->reader = stub_->PrepareAsyncGetDialogueResult(&call->context, request, &cq_);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, (void*)call);
}

void AsyncGrpcClient::BatchUpdateStatusAsync(const std::vector<AgentStatusUpdate>& updates, StatusCallback on_complete) {
    auto* call = new RpcCall<mundusvivens::BatchUpdateAgentStatusResponse>();
    call->client = this;
    call->on_complete = [on_complete](bool ok, mundusvivens::BatchUpdateAgentStatusResponse& resp, const grpc::Status& status) {
        if (ok) {
            on_complete(true, resp.updated_count(), "배치 업데이트 완료");
        } else {
            on_complete(false, 0, "gRPC error: " + status.error_message());
        }
    };

    mundusvivens::BatchUpdateAgentStatusRequest request;
    for (const auto& update : updates) {
        auto* agent_req = request.add_agents();
        agent_req->set_agent_id(update.agent_id);
        agent_req->set_location(update.location);
        agent_req->set_emotion(update.emotion);
        agent_req->set_activity(update.activity);
    }

    call->reader = stub_->PrepareAsyncBatchUpdateAgentStatus(&call->context, request, &cq_);
    call->reader->StartCall();
    call->reader->Finish(&call->response, &call->status, (void*)call);
}

} // namespace MundusVivens
