#include "TcpServer.h"
#include "ClientSession.h"
#include <iostream>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>

TcpServer::TcpServer(boost::asio::io_context& io, uint16_t port)
    : io_(io), acceptor_(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)) {
    // 소켓 주소 재사용 옵션 설정
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
}

TcpServer::~TcpServer() {
    // 모든 세션 정리
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
}

void TcpServer::Start() {
    if (is_running_) return;
    is_running_ = true;

    // AcceptLoop 코루틴 구동
    boost::asio::co_spawn(io_, AcceptLoop(), boost::asio::detached);
    std::cout << "[TCP Server] 포트 " << acceptor_.local_endpoint().port() << "에서 클라이언트 접속 대기 중..." << std::endl;
}

boost::asio::awaitable<void> TcpServer::AcceptLoop() {
    try {
        while (is_running_) {
            // 비동기로 클라이언트 연결 대기 및 수락
            boost::asio::ip::tcp::socket socket = co_await acceptor_.async_accept(boost::asio::use_awaitable);
            
            uint32_t session_idx = 0;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                session_idx = next_session_id_++;
            }

            std::cout << "[TCP Server] 새로운 클라이언트 접속 시도 (세션 인덱스: " << session_idx 
                      << ", 주소: " << socket.remote_endpoint() << ")" << std::endl;

            auto session = std::make_shared<ClientSession>(std::move(socket), *this, session_idx);
            RegisterSession(session);

            // 해당 세션의 비동기 수신 루프를 코루틴으로 구동
            boost::asio::co_spawn(io_, session->Run(), boost::asio::detached);
        }
    } catch (const std::exception& e) {
        std::cerr << "[TCP Server] Accept 루프 예외 발생: " << e.what() << std::endl;
    }
}

void TcpServer::BroadcastPacket(uint16_t packet_id, const std::string& payload) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [index, session] : sessions_) {
        if (session) {
            session->Send(packet_id, payload);
        }
    }
}

void TcpServer::SendTo(uint32_t session_index, uint16_t packet_id, const std::string& payload) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_index);
    if (it != sessions_.end() && it->second) {
        it->second->Send(packet_id, payload);
    }
}

std::shared_ptr<ClientSession> TcpServer::GetSession(uint32_t session_index) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_index);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<PlayerCommand> TcpServer::DrainPlayerCommands() {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    std::vector<PlayerCommand> commands = std::move(pending_commands_);
    pending_commands_.clear();
    return commands;
}

void TcpServer::RegisterSession(std::shared_ptr<ClientSession> session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_[session->GetIndex()] = session;
    std::cout << "[TCP Server] 세션 등록 완료. 현재 세션 수: " << sessions_.size() << std::endl;
}

void TcpServer::UnregisterSession(uint32_t session_index) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_index);
    if (it != sessions_.end()) {
        sessions_.erase(it);
        std::cout << "[TCP Server] 세션 제거 완료 (인덱스: " << session_index 
                  << "). 현재 세션 수: " << sessions_.size() << std::endl;
    }
}

void TcpServer::QueueCommand(PlayerCommand cmd) {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    pending_commands_.push_back(std::move(cmd));
}
