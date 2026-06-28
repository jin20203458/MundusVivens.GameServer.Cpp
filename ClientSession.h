#pragma once
#include <boost/asio.hpp>
#include <memory>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include "PacketProtocol.h"

class TcpServer;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
    ClientSession(boost::asio::ip::tcp::socket socket, TcpServer& server, uint32_t index);
    ~ClientSession();

    // C++20 코루틴 기반 데이터 수신 루프 실행
    boost::asio::awaitable<void> Run();

    // 클라이언트에게 패킷 전송 (스레드 안전)
    void Send(uint16_t packet_id, const std::string& payload);

    uint32_t GetIndex() const { return index_; }
    const std::string& GetPlayerId() const { return player_id_; }
    void SetPlayerId(const std::string& id) { player_id_ = id; }

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
        if (pending_grpc_requests_ >= 32) {
            is_reading_suspended_ = true;
        } else if (pending_grpc_requests_ <= 8) {
            is_reading_suspended_ = false;
        }
    }

    // 수신된 로우 패킷을 처리하여 큐에 명령어로 파싱 및 삽입
    void HandlePacket(uint16_t packet_id, const uint8_t* payload, size_t size);

    // 비동기 전송 처리
    boost::asio::awaitable<void> WriteLoop();

    boost::asio::ip::tcp::socket socket_;
    TcpServer& server_;
    uint32_t index_;
    std::string player_id_;

    // 백프레셔 흐름 제어 상태
    size_t pending_grpc_requests_ = 0;
    bool is_reading_suspended_ = false;

    // 전송 큐 및 동기화 락
    std::mutex write_mutex_;
    std::deque<std::vector<uint8_t>> write_queue_;
    bool is_writing_ = false;
};
