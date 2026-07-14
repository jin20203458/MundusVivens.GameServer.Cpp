#pragma once
#include <mutex>
#include <vector>
#include <functional>
#include <entt/entt.hpp>
#include "TracyIntegration.h"

class TcpServer;
namespace MundusVivens { class AsyncGrpcClient; }

// I/O 스레드 및 gRPC 스레드 -> 메인 스레드로 작업을 안전하게 전달하는 큐
class GrpcResultQueue {
public:
    using Task = std::function<void(entt::registry&, TcpServer&, MundusVivens::AsyncGrpcClient&)>;

    // 백그라운드 스레드에서 호출 (스레드 안전)
    void Push(Task task) {
        ZoneScopedN("Queue Push");
        std::lock_guard<std::mutex> lock(mutex_);
        write_buffer_.push_back(std::move(task));
    }

    // 메인 스레드에서 매 틱마다 호출 (더블 버퍼 스왑으로 락 시간 최소화)
    void Drain(entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
        {
            ZoneScopedN("Queue Drain Swap");
            std::lock_guard<std::mutex> lock(mutex_);
            std::swap(write_buffer_, read_buffer_);
        }
        ZoneScopedN("Queue Drain Dispatch");
        for (auto& task : read_buffer_) {
            if (task) {
                task(reg, tcp, async_client);
            }
        }
        read_buffer_.clear();
    }

private:
    std::mutex mutex_;
    std::vector<Task> write_buffer_;  // 백그라운드 스레드가 Push
    std::vector<Task> read_buffer_;   // 메인 스레드가 Drain
};
