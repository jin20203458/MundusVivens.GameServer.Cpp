#pragma once
#include <boost/asio.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <memory>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <atomic>
#include "PacketProtocol.h"

class TcpServer;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(boost::asio::ip::tcp::socket socket, TcpServer& server, uint32_t index);
    ~ClientSession();

    // C++20 코루틴 기반 데이터 수신 루프 실행
    boost::asio::awaitable<void> Run();

    // 클라이언트에게 패킷 전송 (스레드 안전)
    void Send(uint16_t packet_id, const uint8_t* payload, size_t size);

    uint32_t GetIndex() const { return index_; }

    // 백프레셔 흐름 제어 API
    void IncrementPendingGrpc() {
        pending_grpc_requests_++;
        CheckBackpressure();
    }
    void DecrementPendingGrpc() {
        if (pending_grpc_requests_ > 0) {
            pending_grpc_requests_--;
        }
        CheckBackpressure();
    }

private:
    void CheckBackpressure() {
        bool was_suspended = is_reading_suspended_;
        if (pending_grpc_requests_ >= 32) {
            is_reading_suspended_ = true;
        } else if (pending_grpc_requests_ <= 8) {
            is_reading_suspended_ = false;
        }

        // 백프레셔가 해제되었고 대기 중이었다면 타이머를 취소하여 코루틴을 즉시 깨움 (Zero-polling)
        if (!is_reading_suspended_ && was_suspended) {
            backpressure_timer_.cancel();
        }
    }

    // 수신된 로우 패킷을 처리하여 큐에 명령어로 파싱 및 삽입
    void HandlePacket(uint16_t packet_id, const uint8_t* payload, size_t size);

    // 비동기 전송 처리
    boost::asio::awaitable<void> WriteLoop();

    boost::asio::ip::tcp::socket socket_;
    TcpServer& server_;
    uint32_t index_;

    // 백프레셔 흐름 제어 상태
    std::atomic<size_t> pending_grpc_requests_{0};
    std::atomic<bool> is_reading_suspended_{false};
    boost::asio::steady_timer backpressure_timer_;

    // 전송 큐 (락프리 SPSC Queue)
    boost::lockfree::spsc_queue<PacketBuffer, boost::lockfree::capacity<1024>> write_queue_;
    std::atomic<bool> is_writing_{false};
};
