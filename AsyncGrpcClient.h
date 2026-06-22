#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "mundus_vivens.grpc.pb.h"
#include "MundusVivensClient.h"

namespace MundusVivens {

class AsyncGrpcClient {
public:
    using TickCallback = std::function<void(bool success, const std::string& message, const std::vector<std::string>& busy_agent_ids)>;
    using DialogueCallback = std::function<void(bool success, const DialogueResult& result)>;
    using StatusCallback = std::function<void(bool success, int32_t updated_count, const std::string& message)>;

    AsyncGrpcClient(const std::string& address);
    ~AsyncGrpcClient();

    void ProcessWorldTickAsync(int32_t tick, TickCallback on_complete);
    void TriggerDialogueAsync(const std::string& agent_id_a, const std::string& agent_id_b, DialogueCallback on_complete);
    void PollDialogueResultAsync(const std::string& task_id, DialogueCallback on_complete);
    void BatchUpdateStatusAsync(const std::vector<AgentStatusUpdate>& updates, StatusCallback on_complete);

    // 메인 루프에서 매 틱마다 호출하여 완료된 RPC 결과를 디스패치
    void DrainCompletedResults();

private:
    struct AsyncRpcTag {
        virtual ~AsyncRpcTag() = default;
        virtual void HandleCompletion(bool ok) = 0;
    };

    template <typename TResp>
    struct RpcCall : public AsyncRpcTag {
        grpc::ClientContext context;
        grpc::Status status;
        TResp response;
        std::unique_ptr<grpc::ClientAsyncResponseReader<TResp>> reader;
        std::function<void(bool ok, TResp& resp, const grpc::Status& status)> on_complete;
        AsyncGrpcClient* client;

        void HandleCompletion(bool ok) override {
            // RpcCall 객체가 백그라운드 스레드 루프에서 즉시 delete 되므로,
            // 호출에 필요한 데이터들을 값 복사하여 디스패치 큐로 전달함으로써 Use-After-Free를 원천 방지합니다.
            auto local_on_complete = this->on_complete;
            auto local_response = this->response;
            auto local_status = this->status;

            client->QueueCallback([local_on_complete, ok, local_response, local_status]() {
                if (local_on_complete) {
                    auto resp_copy = local_response;
                    local_on_complete(ok && local_status.ok(), resp_copy, local_status);
                }
            });
        }
    };

    void QueueCallback(std::function<void()> cb);
    void WorkerLoop();

    std::unique_ptr<mundusvivens::MundusVivensGrpc::Stub> stub_;
    grpc::CompletionQueue cq_;
    std::thread cq_worker_;
    std::atomic<bool> shutting_down_;

    std::mutex result_mutex_;
    std::vector<std::function<void()>> completed_callbacks_;
};

} // namespace MundusVivens
